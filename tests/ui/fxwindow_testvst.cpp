// A tiny VST2 effect for fxwindow_test: three settings and a pretend window of its own (it only records what it was asked
// to do), for testing the effects window's hosting of plug-in windows without real plug-ins. Build:
//   g++ -shared -fPIC -std=c++17 -D__cdecl= -I third_party/vst2 tests/ui/fxwindow_testvst.cpp -o fxwintest.so
#include "vst.h"
#include <cstdio>
#include <cstring>

namespace {

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effEditGetRect = 13, effEditOpen = 14,
    effEditClose = 15, effEditIdle = 19, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48
};
enum { amAutomate = 0, amSizeWindow = 15 };

vst_effect_t gFx;
vst_host_callback_t gHost = nullptr;
float gParams[3] = {0.5f, 0.25f, 0.0f};
vst_rect_t gRect = {0, 0, 180, 320};
int gOpens = 0, gCloses = 0, gIdles = 0, gOpenNow = 0;
void* gParent = nullptr;

const char* const kNames[3] = {"Gain", "Mix", "Mode"};

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t*, int32_t op, int32_t index, intptr_t, void* ptr, float)
{
    char* s = (char*)ptr;
    switch (op) {
    case effOpen: case effClose: return 0;
    case effGetParamName: if (s && index >= 0 && index < 3) std::snprintf(s, 32, "%s", kNames[index]); return 0;
    case effGetParamLabel: if (s) std::snprintf(s, 32, "%s", index == 0 ? "dB" : (index == 1 ? "%" : "")); return 0;
    case effGetParamDisplay:
        if (!s || index < 0 || index >= 3) return 0;
        if (index == 0) std::snprintf(s, 32, "%.1f", -24.0 + 48.0 * gParams[0]);
        else if (index == 1) std::snprintf(s, 32, "%d", (int)(gParams[1] * 100.0f + 0.5f));
        else std::snprintf(s, 32, "%s", gParams[2] < 0.5f ? "Soft" : "Hard");
        return 0;
    case effEditGetRect: if (ptr) *(vst_rect_t**)ptr = &gRect; return 1;
    case effEditOpen: ++gOpens; gOpenNow = 1; gParent = ptr; return 1;
    case effEditClose: ++gCloses; gOpenNow = 0; return 1;
    case effEditIdle: ++gIdles; return 1;
    case effGetEffectName: if (s) std::snprintf(s, 64, "FxWin Test Editor"); return 1;
    case effGetVendorString: if (s) std::snprintf(s, 64, "Reno Tests"); return 1;
    case effGetProductString: if (s) std::snprintf(s, 64, "FxWin Test Editor"); return 1;
    default: return 0;
    }
}

void VST_FUNCTION_INTERFACE process(vst_effect_t*, const float* const* in, float** out, int32_t n)
{
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < n; ++k) out[c][k] = in[c][k] * gParams[0];
}

void VST_FUNCTION_INTERFACE setParam(vst_effect_t*, uint32_t i, float v)
{
    if (i < 3) gParams[i] = v;
}

float VST_FUNCTION_INTERFACE getParam(vst_effect_t*, uint32_t i) { return i < 3 ? gParams[i] : 0.f; }

}  // namespace

extern "C" {

#if defined(_WIN32)
#define FXT_EXPORT __declspec(dllexport)
#else
#define FXT_EXPORT __attribute__((visibility("default")))
#endif

FXT_EXPORT vst_effect_t* VSTPluginMain(vst_host_callback_t host)
{
    gHost = host;
    std::memset(&gFx, 0, sizeof(gFx));
    gFx.magic_number = (int32_t)VST_MAGICNUMBER;
    gFx.control = control;
    gFx.process = process;
    gFx.process_float = process;
    gFx.set_parameter = setParam;
    gFx.get_parameter = getParam;
    gFx.num_params = 3;
    gFx.num_inputs = 2;
    gFx.num_outputs = 2;
    gFx.flags = 1 | (1 << 4);           // has an editor, processes in place
    gFx.unique_id = 0x46785774;
    gFx.version = 1;
    return &gFx;
}

// ---- what the test reads and does
FXT_EXPORT int fxwintest_opens() { return gOpens; }
FXT_EXPORT int fxwintest_closes() { return gCloses; }
FXT_EXPORT int fxwintest_idles() { return gIdles; }
FXT_EXPORT int fxwintest_open_now() { return gOpenNow; }
FXT_EXPORT void* fxwintest_parent() { return gParent; }
FXT_EXPORT float fxwintest_param(int i) { return (i >= 0 && i < 3) ? gParams[i] : -1.f; }
// As if the user had moved a knob in the plug-in's window.
FXT_EXPORT void fxwintest_move(int i, float v)
{
    if (i < 0 || i >= 3) return;
    gParams[i] = v;
    if (gHost) gHost(&gFx, amAutomate, i, 0, nullptr, v);
}
// As if the plug-in's window wanted another size.
FXT_EXPORT void fxwintest_resize(int w, int h)
{
    gRect.right = (int16_t)w;
    gRect.bottom = (int16_t)h;
    if (gHost) gHost(&gFx, amSizeWindow, w, h, nullptr, 0.f);
}

}  // extern "C"
