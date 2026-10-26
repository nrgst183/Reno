// Optical flow frame interpolation (VEGAS Pro's Optical Flow resampling and its Slow Motion plug-in): a frame between
// two frames made by finding how the frame moved from one to the other and moving it part of the way, instead of
// blending the two frames.
#pragma once
#include "../core/base.h"

namespace reno {

// The frame a fraction t (0..1) of the way from 'a' to 'b' (same size and depth), into dst. False if they do not fit.
bool interpolateOpticalFlow(Image& dst, const Image& a, const Image& b, float t);

// The flow from a to b as one (dx, dy) per block of 'step' pixels (for tests and the "show flow field" view): gw x gh
// blocks, in pixels of the frame 'a'.
struct FlowField {
    int gw = 0, gh = 0, step = 4;
    std::vector<float> dx, dy;
};
FlowField estimateFlow(const Image& a, const Image& b);

}  // namespace reno
