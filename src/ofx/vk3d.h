// The GPU versions of 3D Shuffle, 3D Blinds, 3D Fly In/Out and 3D Cascade (vk3d.cpp; the kernels are tools/gpukernels_3d.py's). vktrans.cpp lists them (extraGpuEntries) and
// asks here for their GPU check cases. false from a planner: the CPU draws this frame.
#pragma once
#include <string>
#include <vector>
#include "vkeffects.h"

namespace reno {
namespace ofx {
namespace builtin {

const std::vector<ExtraGpuEntry>& threeDGpuEntries();
void threeDGpuCases(const std::string& id, std::vector<GpuCheckCase>& out);
bool threeDGpuFloatTolerance(const std::string& id, double& tol);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
