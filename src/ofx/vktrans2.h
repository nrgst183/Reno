// The GPU versions of the older (DirectX-era) VEGAS transitions that the CPU code draws as VEGAS's own code does: Venetian Blinds, Portals, Gradient Wipe and the four 3D
// transitions (the kernels are tools/gpukernels_trans2.py's, the planners vktrans2.cpp's). vktrans.cpp lists them (extraGpuEntries) and asks here for their GPU check cases.
// false from a planner: the CPU draws this frame.
#pragma once
#include <string>
#include <vector>
#include "vkeffects.h"

namespace reno {
namespace ofx {
namespace builtin {

bool planVenetian(Ctx& c, gpu::Plan& p);
bool planPortals(Ctx& c, gpu::Plan& p);
bool planGradientWipe(Ctx& c, gpu::Plan& p);

// The GPU check cases of the transitions above (id: the full effect id) and their float tolerance (false: not one of them)
void trans2Cases(const std::string& id, std::vector<GpuCheckCase>& out);
bool trans2FloatTolerance(const std::string& id, double& tol);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
