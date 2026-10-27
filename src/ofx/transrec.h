// What the 8-bit transition functions of builtins_trans.cpp (Push, Swap, Squeeze, Zoom, Slide, Spiral, Split, Barn Door) draw, noted down instead of drawn,
// so the GPU planner (vktrans.cpp) runs exactly the CPU code's geometry (rectangles, fixed-point steps, border edges) and only the pixel work is left to the
// kernels. Every blit and border call becomes one Op; the numbers are described where the kernels read them (tools/gpukernels_trans.py, blit8 and border8).
#pragma once
#include <string>
#include <vector>
#include "builtins_impl.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace transrec {

struct Op {
    int kind = 0;           // 0 blit, 1 border
    int src = 0;            // blit: 1 the outgoing picture, 2 the incoming one
    int dst = 0;            // the picture drawn on (1, 2, ...: starts cleared)
    std::vector<int> t;     // the numbers
};
struct Rec {
    int w = 0, h = 0;
    const void* a = nullptr;    // &Ctx::src and &Ctx::src2, to tell the two pictures apart
    const void* b = nullptr;
    std::vector<Op> ops;
    int nextDst = 1;
    int finalDst = 0;
    int resDst[2] = {0, 0};     // Cross Effect's zoom: the two pictures it ends with (a drawn one, dst id, or a source, tag 1 / 2)
    int resTag[2] = {0, 0};
};

// Records what the 8-bit function of the transition with this id (the part after the colon, e.g. "push") draws; false when it has none to record.
bool record(const std::string& shortId, Ctx& c, Rec& r);

// The cells that can hold a pixel for an Iris shape (irisWindowMask in builtins_trans.cpp), for the planner.
uint16_t irisWindow(int shape);
// Star Wipe: the wave position of every pixel of the float picture (row by row from the top), and the mask of the 8-bit picture (bottom row first) with its amplitude.
void starField(Ctx& c, std::vector<double>& field);
void starMask(Ctx& c, std::vector<float>& mask, float& amp);

// The cells that can hold a pixel for an Iris shape (irisWindowMask in builtins_trans.cpp), for the planner.
uint16_t irisWindow(int shape);

}  // namespace transrec
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
