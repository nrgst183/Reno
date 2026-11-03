// Vulkan plans of the text effects (Titles & Text, Credit Roll, Timecode). The glyphs are rasterised on the CPU (GDI/GDI+ on Windows, Reno's own rasteriser and the application's font source
// elsewhere); the CPU code works out everything up to the rasterised layer and hands it over here, and the plans do the per-pixel passes that follow on the card (kernels: tools/gpukernels_text.py).
#pragma once
#include "vkeffects.h"
#include "vfxtimecode.h"
#include <cstdint>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

// builtins_gen.cpp: Titles & Text up to the layer. layer false: the picture is the background alone. Returns false when the CPU has to draw the whole picture (Reno's older moving text).
struct TitleGpu {
    bool layer = false;
    int w = 0, h = 0;
    std::vector<uint8_t> bytes;                 // the layer: premultiplied b g r a, rows top first
    bool blur = false;
    double blurSx = 0, blurSy = 0;
    bool glow = false, glowInner = false;
    double glowAmount = 0, glowColor[4] = {0, 0, 0, 0};      // (alpha first, straight: as the item stores it)
    bool shadow = false;
    double shadowColor[4] = {0, 0, 0, 0}, shadowBlur = 0, shadowDx = 0, shadowDy = 0;
    double opacity = 1;
    float bg[4] = {0, 0, 0, 0};                 // the background as the CPU fills it (premultiplied)
};
bool titlesAndTextGpu(Ctx& c, TitleGpu& g);

// builtins_gen.cpp: Credit Roll up to the rasterised text: the blits (a coverage mask, its place, the clip and the colour) in the order the CPU draws them, then the last pass (the zoom or a wipe of the box)
struct CreditGpu {
    struct Blit {
        int x0 = 0, y0 = 0, w = 0, h = 0, l = 0, t = 0, r = 0, b = 0;
        float col[4] = {0, 0, 0, 0};
        std::vector<uint8_t> a;
    };
    std::vector<Blit> blits;
    int postKind = 0;                           // 0 none, 1 zoom, 2 wipe
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;     // the box (right and bottom excluded)
    double t = 0;
    bool vertical = false, clearFirst = false;
    float bg[4] = {0, 0, 0, 0};
};
bool creditRollGpu(Ctx& c, CreditGpu& g);

// builtins_more_creative.cpp: Timecode's bitmap and its place (false: the picture goes through as it is)
namespace more { bool timecodeGpuLayout(Ctx& c, vfxtc::Layout& L); }

// the planners (vktext.cpp), listed in vktrans.cpp's table with their test cases
bool planTitlesAndText(Ctx& c, gpu::Plan& p);
bool planTimecode(Ctx& c, gpu::Plan& p);
bool planCreditRoll(Ctx& c, gpu::Plan& p);
void textGpuCases(const std::string& fullId, std::vector<GpuCheckCase>& out);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
