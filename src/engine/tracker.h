// Motion tracking (VEGAS's Motion Tracking, Alt+M): following a box over a frame from frame to frame, by template
// matching - the box's frame (in grey) is looked for near where it was in the next frame, the place whose frame
// correlates best (normalised cross-correlation, so changes of brightness do not throw it) being where it went.
#pragma once
#include "../core/base.h"
#include <vector>

namespace reno {

struct GrayImage {
    int w = 0, h = 0;
    std::vector<float> px;                  // top row first, 0..1
    float at(int x, int y) const { return px[(size_t)y * (size_t)w + (size_t)x]; }
};
// A frame in grey (Rec. 709 luma), made at most 'maxW' wide (keeping its shape).
GrayImage toGray(const Image& img, int maxW, double* scale = nullptr);
GrayImage halfSize(const GrayImage& g);

class BoxTracker {
public:
    // The box (centre and size, in pixels of 'first') to follow. False if it is too small or not in the frame.
    bool start(const GrayImage& first, double cx, double cy, double w, double h);
    // Where the box went in 'next' (its centre, updated). False if it could not be found (the centre stays).
    bool step(const GrayImage& next, double& cx, double& cy, double* score = nullptr);
    double searchRadius = 0;                // how far it may have moved from where it was going, in pixels (0: about half the box's size, at least 12)
    double minScore = 0.45;                 // a match poorer than this is "lost"

private:
    int tw_ = 0, th_ = 0;                   // template size (odd)
    std::vector<float> tmpl_;               // the box's frame, mean taken off
    double tnorm_ = 0;                      // its size (square root of the sum of squares)
    double cx_ = 0, cy_ = 0;
    double vx_ = 0, vy_ = 0;                // how far it moved last time (where it is looked for first is ahead by that)
    void setTemplate(const GrayImage& g, double cx, double cy);
    // The correlation of the template at integer top-left (x, y) of 'g' ('t' a template of 'g's scale).
    static double ncc(const GrayImage& g, int x, int y, const std::vector<float>& t, int tw, int th, double tnorm);
};

}  // namespace reno
