// The curves of VEGAS's Color Curves and Color Grading effects as the effects window edits them. VEGAS keeps them as the text of a Custom setting in two formats (decoded from VEGAS's
// presets and projects; read by the built-ins in builtins_magix.cpp and builtins_color.cpp):
//
//   Color Curves "Property"   base64 of: an int 1, the number of points of each of the four curves (red, green, blue, master) as four ints, then every point in 104 bytes: x and y
//                             (doubles, 0..255), 8 bytes of flags, the handle coming in (the handle's share of the gap to the previous point, its slope, the offset dx dy, an int: whether
//                             there is a handle) and the same for the handle going out.
//   Color Grading Curve_R/G/B/Y   "N:" and then six numbers per point separated by ':' - x, y, the handle going back (dx, dy) and the one going forward (dx, dy), all in 0..1.
//
// The curve between two points is the cubic Bezier with control points at point + out-handle and next point + in-handle. A handle is kept as VEGAS keeps it, as a share of the gap to its
// neighbour and a slope (dx = share x gap, dy = slope x dx), so moving a point moves its neighbours' handles with it.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace reno {
namespace ofx {

struct BezPoint {
    double x = 0, y = 0;
    double inShare = 0.2, inSlope = 1.0, outShare = 0.2, outSlope = 1.0;
    bool inOn = true, outOn = true;
    bool smooth = true;
    double rawIn[2] = {0, 0}, rawOut[2] = {0, 0};
    unsigned char flags[8] = {1, 0, 1, 1, 1, 1, 0, 0};
};
typedef std::vector<BezPoint> BezCurve;

// The handle offsets of point i (dx, dy from the point): they follow from the shares and the gaps to the neighbours.
void bezHandleIn(const BezCurve& c, size_t i, double* dx, double* dy);
void bezHandleOut(const BezCurve& c, size_t i, double* dx, double* dy);
// Moves a handle to the offset (dx, dy) from its point, within its gap; with 'smooth' the other handle turns to the same slope.
void bezSetHandleIn(BezCurve& c, size_t i, double dx, double dy);
void bezSetHandleOut(BezCurve& c, size_t i, double dx, double dy);
// Puts a new point at (x, y) (between the points whose x it lies between) with handles in the direction of its neighbours.
// 'range' is the curve's full scale (255 or 1). Gives its index.
size_t bezInsert(BezCurve& c, double x, double y, double range);
// Takes point i away (not the first or the last: false). The neighbours keep their handles.
bool bezRemove(BezCurve& c, size_t i);
// Moves point i to (x, y), kept between its neighbours (the ends only up and down).
void bezMove(BezCurve& c, size_t i, double x, double y, double range);
// The curve as a polyline (stepsPerSegment points on each piece), x never going back.
void bezPolyline(const BezCurve& c, int stepsPerSegment, std::vector<std::pair<double, double>>& out);
// A curve the user has edited, set down the way VEGAS writes one it has edited itself (the flags of the points, which of them
// have handles in and out).
void bezNormalise(BezCurve& c);
// Two points on a line, 0..range.
BezCurve bezStraight(double range);

// Color Curves' "Property": four curves, 0..255. Decode gives false for text that is no set of curves.
bool decodeColorCurves(const std::string& text, BezCurve out[4]);
std::string encodeColorCurves(const BezCurve in[4]);

// Color Grading's Curve_R ...: 0..1.
bool decodeGradingCurve(const std::string& text, BezCurve& out);
std::string encodeGradingCurve(const BezCurve& in);

}  // namespace ofx
}  // namespace reno
