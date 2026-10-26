// Effects built into Reno that need no plug-in (FxSlot::kind "builtin"): the engine applies them itself. Their
// settings are the slot's 'state' (saved with the project like any effect's), changed through AppState::edit, so
// every change is an ordinary undo step.
//
// "reno.colorgrade": VEGAS's Color Grading panel (Alt+G). In order:
//   exposure            stops (2^exposure)
//   temperature, tint   -100 .. 100: white balance (warmer / cooler; magenta / green)
//   lift, gamma, gain, offset   the colour wheels: each { "x", "y" } a point in the wheel (the colour it pushes
//                       towards, radius 0..1) and "master" (lift and offset: added, -1..1; gamma and gain: factors);
//                       out = (gain x (c + lift x (1 - c)))^(1 / gamma) + offset, per channel
//   contrast            about mid grey (1 = none), saturation (1 = none, Rec. 709 luma kept)
//   curves              "master", "r", "g", "b": lists of [x, y] points (0..1) through which a smooth curve runs
// Colour is worked on unpremultiplied, in the project's depth (8-bit frames are clipped to 0..1 at the end; float
// frames in a 32-bit project are not).
#pragma once
#include "project.h"
#include <memory>

namespace reno {

extern const char* const kColorGradeId;     // "reno.colorgrade"

// Whether a slot is a video effect the engine runs (an OpenFX effect, or a built-in one).
bool isVideoFxSlot(const FxSlot& s);
bool isBuiltinVideo(const FxSlot& s);
// The name shown for a built-in effect.
std::string builtinLabel(const std::string& id);
// A new slot of a built-in effect, with its settings at their defaults.
FxSlot makeBuiltinSlot(const std::string& id, uint64_t slotId);
// Applies a built-in effect to a frame (in place). False if the id is unknown (the frame is left alone).
bool applyBuiltin(const FxSlot& s, Image& img);

// A 3D colour look-up table (VEGAS's LUTs: .cube files, and Reno's own camera and look presets): n x n x n colours, the
// red index changing fastest.
struct Lut3D {
    int n = 0;
    std::vector<float> rgb;             // n^3 x 3
    double domainMin[3] = {0, 0, 0}, domainMax[3] = {1, 1, 1};
    // One colour (straight RGB, 0..1) through the table, trilinear or tetrahedral.
    void apply(float c[3], bool tetrahedral) const;
};
// Reads a .cube file (3D tables; 1D tables are turned into 3D ones). Null with *error if it is not one.
std::shared_ptr<const Lut3D> readCubeLut(const std::string& path, std::string* error);
// Reno's own LUTs, for the Color Grading panel's LUT lists: a camera (input) transform or a creative look, each a
// function of one colour.
struct LutPreset {
    std::string id, label;
    bool camera = false;
    void (*fn)(float rgb[3]);
};
const std::vector<LutPreset>& lutPresets();
// A table of 'n' points a side sampled from a preset.
std::shared_ptr<const Lut3D> lutOfPreset(const std::string& id, int n = 33);

// One LUT slot of the colour grade: where it comes from ("" none, "preset:<id>" or "file:<path>") and how it is used.
struct LutRef {
    std::string source;
    double strength = 1.0;              // 0 .. 1: how much of the LUT's result is mixed in
    bool tetrahedral = false;
    bool enabled = true;                // "LUT selection": the toggle that auditions the chosen LUT
    bool operator==(const LutRef& o) const { return source == o.source && strength == o.strength && tetrahedral == o.tetrahedral && enabled == o.enabled; }
    bool active() const { return !source.empty() && enabled && strength > 1e-9; }
};

// The colour grade's settings, read from / written to a slot's state.
//   Order of work: Camera LUT, exposure / levels in / tone / white balance / HSL (Utilities), Color Wheels, RL Color
//   Wheels, saturation, curves, levels out, HSL curves, Look LUT.
struct ColorGrade {
    struct Wheel { double x = 0, y = 0, master = 0; };
    double exposure = 0, temperature = 0, tint = 0;
    Wheel lift, gamma{0, 0, 1}, gain{0, 0, 1}, offset;
    double contrast = 1, saturation = 1;
    std::vector<std::pair<double, double>> curve[4];    // master, r, g, b (empty = straight)
    // --- VEGAS Pro 23's Color Grading panel
    LutRef cameraLut, lookLut;
    // RL Color Wheels: range-limited wheels (their master is a luminance amount, 0 = none) and the two thresholds (0..1
    // luma) that separate shadows, midtones and highlights; offset acts everywhere, rolling off at the ends.
    Wheel shadows, midtones, highlights, rlOffset;
    double rlLow = 0.25, rlHigh = 0.75;
    // Utilities: Levels (input and output black and white), Tone (brightness, contrast pivot, log exposure) and HSL
    double blackIn = 0, whiteIn = 1, blackOut = 0, whiteOut = 1;
    double brightness = 0, contrastCenter = 0.5, logExposure = 0;
    double hue = 0, hslSaturation = 1, luminance = 0;    // hue -1..1 of a turn; saturation a factor; luminance -1..1
    // HSL curves (points x 0..1, y 0..1 with 0.5 = no change), in this order:
    enum HslCurve { HueVsHue, HueVsSat, HueVsLum, SatVsSat, SatVsLum, LumVsSat, HslCurveCount };
    std::vector<std::pair<double, double>> hslCurve[HslCurveCount];
    static ColorGrade fromJson(const Json& j);
    Json toJson() const;
    bool isIdentity() const;
    // One colour (straight, 0..1 in, any out) through the grade.
    void apply(float rgb[3]) const;
    // (prepared tables for apply: the curves and LUTs sampled)
    void prepare();
    // The Color Grading panel's pickers: from a colour picked in the frame (straight RGB, 0..1) the settings that
    // make it black (Lift), white (Gain), or neutral (Temperature and Tint).
    void setBlackPoint(const float rgb[3]);
    void setWhitePoint(const float rgb[3]);
    void setNeutralTone(const float rgb[3]);
private:
    std::vector<float> lut_[4];
    std::vector<float> hslLut_[HslCurveCount];
    std::shared_ptr<const Lut3D> camera_, look_;
    bool prepared_ = false;
    bool hasHsl_ = false, hasRl_ = false;
};

// Red Eye Reduction (MediaItem::redEye): inside each area (an ellipse filling it, its edge softened) the red of
// pixels that are much redder than they are green or blue is brought down to the level of the other two.
ImagePtr applyRedEye(const ImagePtr& img, const std::vector<MediaItem::RedEyeArea>& areas);

// The curve through 'points' (sorted by x, kept to 0..1) at x: a monotone cubic (it does not overshoot).
double curveThrough(const std::vector<std::pair<double, double>>& points, double x);

}  // namespace reno
