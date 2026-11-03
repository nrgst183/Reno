// The planners of the transitions as VEGAS draws them with its OpenCL kernels (Ctx::openCl): the GPU versions of the CPU ports in vfxwipes.h, vfxswap.h, ... (the kernels are
// tools/gpukernels_oclt.py's). vktrans.cpp calls them in place of its older plans when Ctx::openCl is on. false: the CPU draws this frame.
#pragma once
#include "vkeffects.h"

namespace reno {
namespace ofx {
namespace builtin {

bool planOclDissolve(Ctx& c, gpu::Plan& p);
bool planOclBarndoor(Ctx& c, gpu::Plan& p);
bool planOclPush(Ctx& c, gpu::Plan& p);
bool planOclSwap(Ctx& c, gpu::Plan& p);
bool planOclSlide(Ctx& c, gpu::Plan& p);
bool planOclSpiral(Ctx& c, gpu::Plan& p);
bool planOclZoom(Ctx& c, gpu::Plan& p);
bool planOclSqueeze(Ctx& c, gpu::Plan& p);
bool planOclSplit(Ctx& c, gpu::Plan& p);
bool planOclFlash(Ctx& c, gpu::Plan& p);
bool planOclCrossEffect(Ctx& c, gpu::Plan& p);
bool planOclStarWipe(Ctx& c, gpu::Plan& p);
bool planOclIris(Ctx& c, gpu::Plan& p);
bool planOclPagePeel(Ctx& c, gpu::Plan& p);
bool planOclPageLoop(Ctx& c, gpu::Plan& p);
bool planOclPageRoll(Ctx& c, gpu::Plan& p);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
