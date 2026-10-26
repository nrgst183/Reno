// Built-in versions of VEGAS Pro 18's Stabilize, Motion Tracker, Flicker Reducer, the denoisers, Colorization and Style Transfer (the "analysis" group), with identifiers and
// parameter names read from the plug-ins' own code (Stabilize.ofx, MagixCVFx.ofx, MagixAiFx.ofx; see tools/analysis_params.txt):
//
//   com.vegascreativesoftware:Stabilize   VEGAS Stabilize Video (the old Stabilize plug-in)     builtins_analysis_stab.cpp
//   de.magix:Stabilize                    VEGAS Video Stabilization (the new one)               builtins_analysis_stab.cpp
//   de.magix:MotionTracker                VEGAS Motion Tracker                                  here
//   de.magix:FlickerReducer               VEGAS Flicker Control                                 builtins_analysis_flicker.cpp
//   de.magix:DenoisingNLM                 VEGAS Denoise                                         builtins_analysis_nr.cpp
//   com.sonycreativesoftware:smart_noise_reduction   Smart Noise Reduction                      builtins_analysis_nr.cpp
//   de.magix:Colorization                 VEGAS Colorization (AI in VEGAS)                      builtins_analysis_ai.cpp
//   de.magix:StyleTransfer                VEGAS Style Transfer (AI in VEGAS)                    builtins_analysis_ai.cpp
//
// The motion, flicker and temporal effects look at the frames before and after the one they draw (Ctx::fetch) and cache what they found per instance, so the first frame after a seek
// costs a window of frames and playing on costs one more frame each time.
#include "builtins_analysis_internal.h"
#include <algorithm>
#include <cmath>

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {

const char* const kGroup = "Reno Built-in";

Def make(const char* id, const char* label, const char* what, std::vector<PDef> params, void (*fn)(Ctx&))
{
    Def d;
    d.id = id;
    d.label = label;
    d.group = kGroup;
    d.description = std::string(what) + " (Reno's own, standing in for VEGAS " + label + ").";
    d.params = std::move(params);
    d.render = fn;
    return d;
}

namespace {

// ------------------------------------------------------------------ Motion Tracker
// VEGAS's Motion Tracker effect is where the Motion Tracking panel keeps its results (the tracked region's four corners, the vectors found, the model and direction chosen); it does not
// change the picture. With "Show motion vectors" it draws the tracked region. Reno keeps the settings as VEGAS does (so a project round-trips) but cannot read VEGAS's MotionVectors data,
// so it draws the region only; Reno's own Motion Tracking window (Alt+M) does the tracking.
void drawLine(Buf& b, double x0, double y0, double x1, double y1, double width, const float col[3])
{
    const double len = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
    const int steps = (int)std::min(20000.0, std::ceil(len)) + 1;
    const int r = std::max(0, (int)std::floor(width / 2));
    for (int i = 0; i <= steps; ++i) {
        const double t = (double)i / steps;
        const int cx = (int)std::lround(x0 + (x1 - x0) * t), cy = (int)std::lround(y0 + (y1 - y0) * t);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= b.w || y >= b.h) continue;
                float* p = b.at(x, y);
                p[0] = col[0]; p[1] = col[1]; p[2] = col[2]; p[3] = 1.f;
            }
    }
}

void motionTracker(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.out.px.size() == c.src.px.size()) c.out.px = c.src.px;
    if (!c.b("ShowMotionVectors") || W <= 0 || H <= 0) return;
    double cx[4], cy[4];
    static const char* const names[4] = {"CornerTL", "CornerTR", "CornerBR", "CornerBL"};
    for (int k = 0; k < 4; ++k) {
        c.xy(names[k], cx[k], cy[k]);
        if (!(cx[k] >= 0 && cx[k] <= 1 && cy[k] >= 0 && cy[k] <= 1)) return;       // no region set (VEGAS leaves -1, -1)
    }
    const float green[3] = {0.1f, 1.f, 0.2f};
    const double wdt = std::max(1.0, c.px(2.0));
    for (int k = 0; k < 4; ++k) {
        const int n = (k + 1) % 4;
        drawLine(c.out, cx[k] * W, (1 - cy[k]) * H, cx[n] * W, (1 - cy[n]) * H, wdt, green);
    }
}

}  // namespace

void addFiltersAnalysisImpl(std::vector<Def>& out)
{
    addStabilizeEffects(out);
    {
        std::vector<PDef> ps = {intp("Identifier", "Identifier", 0, 0, 1000000), boolean("MaskInvert", "Mask invert", false), boolean("HasMotionData", "Has motion data", false),
                                point("CornerTL", "Top left", -1, -1), point("CornerTR", "Top right", -1, -1), point("CornerBR", "Bottom right", -1, -1),
                                point("CornerBL", "Bottom left", -1, -1), custom("MotionVectors", "Motion vectors", ""), intp("Tracker", "Tracker", 0, 0, 100),
                                intp("NoPoints", "Number of points", 36, 0, 100000), intp("MotionModel", "Motion model", 8, 0, 100), intp("Direction", "Direction", 1, 0, 2),
                                intp("Status", "Status", 0, 0, 100), boolean("ShowMotionVectors", "Show motion vectors", false)};
        for (PDef& p : ps) if (p.name != "ShowMotionVectors") p.secret = true;
        out.push_back(make("de.magix:MotionTracker", "Motion Tracker", "The region the Motion Tracking panel follows, drawn over the picture on request", ps, motionTracker));
    }
    addFlickerEffect(out);
    addDenoiseEffects(out);
    addAiEffects(out);
}

}  // namespace analysis

void addFiltersAnalysis(std::vector<Def>& out)
{
    analysis::addFiltersAnalysisImpl(out);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
