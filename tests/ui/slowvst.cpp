// Stand-in VST2 effects for splash_test: plug-ins that take their time to load or start, and one that crashes. Built several
// times by the test's build line, with:
//   -DSLOWVST_NAME='"Slow Verb"'  its name           -DSLOWVST_UID=0x534C5701  its id
//   -DSLOWVST_MAIN_MS=n   VSTPluginMain sleeps n ms (every time it is loaded: when checked and when used)
//   -DSLOWVST_ON_MS=n     switching it on (effMainsChanged 1) sleeps n ms (only when used)
//   -DSLOWVST_CRASH       VSTPluginMain kills its own process, as a crash does (a sanitizer can't catch SIGKILL, so the
//                         checking process really dies of a signal)
//   g++ -shared -fPIC -std=c++17 -D__cdecl= -I third_party/vst2 tests/ui/slowvst.cpp -o slow.so
#include "vst.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#ifdef SLOWVST_CRASH
#include <signal.h>
#include <unistd.h>
#endif

#ifndef SLOWVST_NAME
#define SLOWVST_NAME "Slow Effect"
#endif
#ifndef SLOWVST_UID
#define SLOWVST_UID 0x534C5700
#endif
#ifndef SLOWVST_MAIN_MS
#define SLOWVST_MAIN_MS 0
#endif
#ifndef SLOWVST_ON_MS
#define SLOWVST_ON_MS 0
#endif

namespace {

enum { effOpen = 0, effClose = 1, effMainsChanged = 12, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48 };

struct Slow {
    vst_effect_t fx;
    float gain = 1.f;
};

void sleepMs(int ms)
{
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t*, int32_t op, int32_t, intptr_t value, void* ptr, float)
{
    char* s = (char*)ptr;
    switch (op) {
    case effMainsChanged: if (value) sleepMs(SLOWVST_ON_MS); return 0;
    case effGetEffectName: case effGetProductString: if (s) std::snprintf(s, 64, "%s", SLOWVST_NAME); return 1;
    case effGetVendorString: if (s) std::snprintf(s, 64, "%s", "Reno Tests"); return 1;
    default: return 0;
    }
}

void VST_FUNCTION_INTERFACE process(vst_effect_t* e, const float* const* in, float** out, int32_t n)
{
    const Slow* s = (const Slow*)e->effect_internal;
    for (int c = 0; c < 2; ++c)
        if (in && out && in[c] && out[c])
            for (int k = 0; k < n; ++k) out[c][k] = in[c][k] * s->gain;
}

void VST_FUNCTION_INTERFACE setParam(vst_effect_t* e, uint32_t i, float v)
{
    if (i == 0) ((Slow*)e->effect_internal)->gain = v;
}

float VST_FUNCTION_INTERFACE getParam(vst_effect_t* e, uint32_t i)
{
    return i == 0 ? ((Slow*)e->effect_internal)->gain : 0.f;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) vst_effect_t* VSTPluginMain(vst_host_callback_t)
{
#ifdef SLOWVST_CRASH
    kill(getpid(), SIGKILL);
#endif
    sleepMs(SLOWVST_MAIN_MS);
    Slow* s = new Slow();  // never freed: a host may still hold the pointer after effClose
    std::memset(&s->fx, 0, sizeof(s->fx));
    s->fx.magic_number = (int32_t)VST_MAGICNUMBER;
    s->fx.control = control;
    s->fx.process = process;
    s->fx.process_float = process;
    s->fx.set_parameter = setParam;
    s->fx.get_parameter = getParam;
    s->fx.num_params = 1;
    s->fx.num_inputs = 2;
    s->fx.num_outputs = 2;
    s->fx.flags = 1 << 4;       // processes in place
    s->fx.unique_id = SLOWVST_UID;
    s->fx.version = 1;
    s->fx.effect_internal = s;
    return &s->fx;
}
