// Built-in stand-in for VEGAS Pro 18's Dual Fish Eye Stitching (manual: "360° video", "Stitching"): the frame of a 360° camera with two fish-eye lenses side by side (front lens's circle
// in the left half, back lens's in the right) made into one equirectangular 360° frame, the seam blended and the vignetting towards each circle's edge removed. Uses VEGAS's identifier and
// settings (from its OfxStitch bundle: gui.xml gives ranges and defaults, VrPresets.xml the camera models). The picture follows VEGAS's own 8-bit code.
#include "builtins_impl.h"
#include <algorithm>
#include <cmath>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

const char* const kPre = "mx.ofx.360.stitch.fisheye.";
std::string pn(const char* n) { return std::string(kPre) + n; }

const float kPif = 3.1415927410125732f, kHalfPif = 1.5707963705062866f;

// One lens's half of the picture: the fisheye in 8 bits (R G B, rows counted from the bottom of the frame) and its settings.
struct Fish {
    int W = 0, Hs = 0;
    std::vector<unsigned char> img;
    float fov = 0, p0 = 0, p1 = 0, inten = 0, rng = 0;
    float ca[2] = {1, 1}, sa[2] = {0, 0};
};

// Where in the fisheye the direction (A, B, C) lies: the position (sx, sy, -1..1) and how far out (RR, 0..1).
void lensCoords(const Fish& f, float A, float B, float C, int lens, bool first, float& sx, float& sy, float& RR)
{
    const float hy = hypotf(C, B), th = atan2f(hy, A);
    const float u = (th + th) / f.fov;
    const float sh = u * f.p0;
    const float q = (u - sh) * f.p1;
    RR = (float)std::fabs((double)q);
    const float ph = atan2f(B, C);
    const float qx = cosf(ph) * q, qy = sinf(ph) * q;
    const float nx = qx * f.ca[lens] - qy * f.sa[lens], ny = qy * f.ca[lens] + qx * f.sa[lens];
    sx = first ? ((nx + 1.f) + sh) * 0.5f : ((nx - 1.f) - sh) * 0.5f;
    sy = ny;
}

// The colour (0..255) the fisheye has at (sx, sy), with the vignetting taken out.
void sampleFish(const Fish& f, float sx, float sy, float RR, int out[3])
{
    sx = sx > -1.f ? (1.f > sx ? sx : 1.f) : -1.f;
    sy = sy > -1.f ? (1.f > sy ? sy : 1.f) : -1.f;
    const float h1 = (float)(f.Hs - 1), w1 = (float)(f.W - 1);
    float fx = ((sx + 1.f) * 0.5f) * w1;
    const float ty = ((sy + 1.f) * 0.5f) * h1;
    float fy = h1 - (h1 - ty);
    fx = fx * 1.f + -0.5f;
    fy = fy * 1.f + -0.5f;
    const float xi = std::floor(fx), yi = std::floor(fy);
    const float fxr = fx - xi, fyr = fy - yi;
    const int x0 = (int)(xi + 0.5f), y0 = (int)(yi + 0.5f);
    const int x1 = std::min(x0 + 1, f.W - 1), y1 = std::min(y0 + 1, f.Hs - 1);
    const float omx = 1.f - fxr, omy = 1.f - fyr;
    const unsigned char *p00 = &f.img[((size_t)y0 * f.W + x0) * 3], *p01 = &f.img[((size_t)y0 * f.W + x1) * 3];
    const unsigned char *p10 = &f.img[((size_t)y1 * f.W + x0) * 3], *p11 = &f.img[((size_t)y1 * f.W + x1) * 3];
    for (int k = 0; k < 3; ++k) {
        const float a = ((float)p00[k] * omx) * omy, b = ((float)p01[k] * fxr) * omy;
        const float c = ((float)p10[k] * omx) * fyr, d = ((float)p11[k] * fxr) * fyr;
        out[k] = (int)((((a + b) + c) + d) + 0.5f) & 0xff;
    }
    if (f.inten > 0.f && f.rng != 0.f) {
        const float om = 1.f - f.rng;
        if (RR >= om) {
            const float tt = (RR - om) / f.rng;
            const float gexp = 1.f - (tt * f.inten) * tt;
            for (int k = 0; k < 3; ++k) {
                float q = powf((float)out[k] / 255.f, gexp);
                q = 0.f > q ? 0.f : (q > 1.f ? 1.f : q);
                out[k] = std::min((int)(q * 255.f + 0.5f), 255);
            }
        }
    }
}

void dualFishEye(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W < 2 || H < 2 || c.src.w != W || c.src.h != H) { c.out.px = c.src.px; return; }
    Fish f;
    const float fov = ((float)c.d(pn("fov").c_str()) * kPif) / 180.f;
    if (!(fov > 0.f)) { c.out.px = c.src.px; return; }
    f.fov = fov;
    f.inten = c.b(pn("falloff.use").c_str()) ? (float)(c.d(pn("falloff.factor").c_str()) / 100.0) : 0.f;
    f.rng = (float)(c.d(pn("falloff.range").c_str()) / 100.0);
    f.p0 = (float)(c.d(pn("center").c_str()) / 100.0);
    f.p1 = (float)(c.d(pn("narrowing").c_str()) / 100.0);
    const float rot = (float)(c.d(pn("rotation").c_str()) / 100.0);
    const float rotrad = (rot * kPif) / 180.f;
    // the orientations as VEGAS numbers them inside: 0, 90, 180, -90 degrees for the entries 0 to 3 of the list
    static const int kMap[4] = {0, 3, 2, 1};
    const int oL = kMap[std::min(3, std::max(0, c.i(pn("orient.left").c_str())))];
    const int oR = kMap[std::min(3, std::max(0, c.i(pn("orient.right").c_str())))];
    const int orient[2] = {oR, oL};
    for (int l = 0; l < 2; ++l) {
        float ang = rotrad;
        if (orient[l] == 1) ang = rotrad + kHalfPif;
        else if (orient[l] == 2) ang = rotrad + kPif;
        else if (orient[l] == 3) ang = rotrad - kHalfPif;
        f.ca[l] = cosf(ang);
        f.sa[l] = sinf(ang);
    }
    // the half of the frame the lenses are in: the top W/2 rows of a wide frame (rows counted from the bottom, so the last ones)
    const int ecx = W >> 1;
    int base = 0;
    f.Hs = H;
    if (ecx < H) { base = H - ecx; f.Hs = ecx; }
    f.W = W;
    f.img.resize((size_t)f.Hs * W * 3);
    for (int m = 0; m < f.Hs; ++m)
        for (int x = 0; x < W; ++x) {
            const float* p = c.src.at(x, H - 1 - (base + m));
            unsigned char* q = &f.img[((size_t)m * W + x) * 3];
            for (int k = 0; k < 3; ++k) q[k] = (unsigned char)(int)(clampd(p[k], 0, 1) * 255.0 + 0.5);
        }
    const float ov = (float)std::fabs((double)sinf((fov - kPif) * 0.5f));
    forRows(H, W, [&](int y) {
        const int m = H - 1 - y;                                  // the row counted from the bottom
        float v = (float)m / (float)(H - 1);
        v = (v + v) - 1.f;
        const float lat = v * kHalfPif;
        float cl = cosf(lat);
        if (std::fabs(cl) < 1e-6f) cl = 0.f;                      // (at the poles the cosine is exactly nothing for VEGAS)
        const float B = sinf(lat);
        for (int x = 0; x < W; ++x) {
            float u = (float)x / (float)(W - 1);
            u = (u + u) - 1.f;
            const float lon = u * kPif;
            const float A = cosf(lon) * cl, C = sinf(lon) * cl;
            const float absA = (float)std::fabs((double)A);
            int r = 0, g = 0, b = 0;
            if (absA < ov) {
                const float ratio = absA / ov;
                const float w1 = A > 0.f ? (ratio + 1.f) * 0.5f : (A < 0.f ? (1.f - ratio) * 0.5f : 0.5f);
                const float w2 = 1.f - w1;
                int c1[3], c2[3];
                float sx, sy, RR;
                lensCoords(f, A, B, C, 0, true, sx, sy, RR);
                sampleFish(f, sx, sy, RR, c1);
                lensCoords(f, -A, B, -C, 1, false, sx, sy, RR);
                sampleFish(f, sx, sy, RR, c2);
                float Y[2], U[2], V[2];
                for (int s = 0; s < 2; ++s) {
                    const int* cc = s == 0 ? c1 : c2;
                    const float fr = (float)cc[0] / 255.f, fg = (float)cc[1] / 255.f, fb = (float)cc[2] / 255.f;
                    Y[s] = ((fg * 0.587f) + (fr * 0.299f)) + (fb * 0.114f);
                    V[s] = ((fr - Y[s]) / 0.701f) * 0.615f;
                    U[s] = ((fb - Y[s]) / 0.886f) * 0.436f;
                }
                const float Yb = (Y[1] * w2) + (Y[0] * w1), Vb = (V[1] * w2) + (V[0] * w1), Ub = (U[1] * w2) + (U[0] * w1);
                float Rb = (Vb * 1.14f) + Yb, Gb = (Yb - (Ub * 0.395f)) - (Vb * 0.581f), Bb = (Ub * 2.033f) + Yb;
                Rb = 0.f > Rb ? 0.f : (Rb > 1.f ? 1.f : Rb);
                Gb = 0.f > Gb ? 0.f : (Gb > 1.f ? 1.f : Gb);
                Bb = 0.f > Bb ? 0.f : (Bb > 1.f ? 1.f : Bb);
                r = std::min((int)(Rb * 255.f + 0.5f), 255);
                g = std::min((int)(Gb * 255.f + 0.5f), 255);
                b = std::min((int)(Bb * 255.f + 0.5f), 255);
            } else {
                int cc[3];
                float sx, sy, RR;
                if (A > ov) {
                    lensCoords(f, A, B, C, 0, true, sx, sy, RR);
                } else {
                    lensCoords(f, absA, B, -C, 1, false, sx, sy, RR);
                }
                sampleFish(f, sx, sy, RR, cc);
                r = cc[0]; g = cc[1]; b = cc[2];
            }
            float* o = c.out.at(x, y);
            o[0] = (float)r / 255.f;
            o[1] = (float)g / 255.f;
            o[2] = (float)b / 255.f;
            o[3] = 1.f;
        }
    });
}

// VEGAS's camera models (its VrPresets.xml): field of view, vignetting removal (intensity and range, per cent), the way each
// fisheye is turned (0 = 0 degrees, 1 = -90, 2 = 180, 3 = 90), the offset of the centres, the radius (per cent) and the rotation.
struct Camera { double fov; bool falloff; double intensity, range; int left, right; double center, narrowing, rotation; };
const Camera kCameras[5] = {
    {191.357990, true, 34.0, 41.5, 0, 0, 0.0, 100.0, 0.0},     // Samsung Gear 360
    {192.4, false, 20.0, 40.0, 0, 0, 0.0, 100.0, 0.0},         // LG 360 Cam
    {191.0, true, 20.0, 30.0, 1, 3, 0.0, 100.0, 0.0},
    {202.50, true, 12.0, 30.0, 1, 3, 0.0, 100.0, 0.0},         // Ricoh Theta S
    {191.0, true, 35.0, 44.0, 1, 1, 0.0, 96.3, 0.0},           // Ricoh Theta
};

// Choosing a camera in "Model" sets the other settings to its values.
void stitchChanged(Change& ch)
{
    if (ch.param != pn("presets")) return;
    const int m = ch.i(pn("presets").c_str());
    if (m < 1 || m > 5) return;
    const Camera& k = kCameras[m - 1];
    ch.setD(pn("fov").c_str(), k.fov);
    ch.setI(pn("falloff.use").c_str(), k.falloff ? 1 : 0);
    if (k.falloff) {
        ch.setD(pn("falloff.factor").c_str(), k.intensity);
        ch.setD(pn("falloff.range").c_str(), k.range);
    }
    ch.setI(pn("orient.left").c_str(), k.left);
    ch.setI(pn("orient.right").c_str(), k.right);
    ch.setD(pn("center").c_str(), k.center);
    ch.setD(pn("narrowing").c_str(), k.narrowing);
    ch.setD(pn("rotation").c_str(), k.rotation);
}

}  // namespace

void addFiltersVr(std::vector<Def>& out)
{
    Def d;
    d.id = "com.magix.ofx.vr.dual.stich";      // VEGAS's identifier, spelt as VEGAS spells it
    d.label = "Dual Fish Eye Stitching";
    d.group = kGroup;
    d.description = "The two fish-eye circles of a 360\xC2\xB0 camera's picture made into one equirectangular 360\xC2\xB0 picture "
                    "(Reno's own, standing in for VEGAS Dual Fish Eye Stitching).";
    const std::vector<std::string> turns = {"0 Degree", "-90 Degrees", "180 Degrees", "90 Degrees"};
    d.params = {choice(pn("presets"), "Model", 0, {"User Defined", "Samsung Gear 360\xC2\xB0", "LG 360\xC2\xB0 Cam", "Ricoh Theta V", "Ricoh Theta S", "Ricoh Theta"}),
                dbl(pn("fov"), "Field of view", 180, 180, 220),
                choice(pn("orient.left"), "Left fisheye", 0, turns),
                choice(pn("orient.right"), "Right fisheye", 0, turns),
                group("mx.ofx.360.stitch.group0_0", "Image Vignetting"),
                boolean(pn("falloff.use"), "Remove vignetting", true, "mx.ofx.360.stitch.group0_0"),
                dbl(pn("falloff.factor"), "Intensity", 20, 0, 100, "mx.ofx.360.stitch.group0_0"),
                dbl(pn("falloff.range"), "Range", 40, 0, 100, "mx.ofx.360.stitch.group0_0"),
                group("mx.ofx.360.stitch.group1_0", "Refinement"),
                dbl(pn("center"), "Center", 0, 0, 10, "mx.ofx.360.stitch.group1_0"),
                dbl(pn("narrowing"), "Radius", 100, 70, 100, "mx.ofx.360.stitch.group1_0"),
                dbl(pn("rotation"), "Rotation", 0, -100, 100, "mx.ofx.360.stitch.group1_0")};
    for (auto& p : d.params) {
        if (p.name == pn("presets")) p.hint = "You can select a preset for a certain camera.";
        if (p.name == pn("fov")) p.hint = "The field of view is the extent of the observable world that is seen at any given moment.";
        if (p.name == pn("orient.left")) p.hint = "Select the orientation of the left fisheye in your raw footage.";
        if (p.name == pn("orient.right")) p.hint = "Select the orientation of the right fisheye in your raw footage.";
        if (p.name == pn("falloff.use")) p.hint = "Vignetting is a reduction of an image's brightness or saturation toward the periphery compared to the image center.";
        if (p.name == pn("falloff.factor")) p.hint = "The maximum intensity to increase toward the periphery.";
        if (p.name == pn("falloff.range")) p.hint = "The radial distance from the periphery to treat.";
        if (p.name == pn("center")) p.hint = "The offset of the center of each fisheye.";
        if (p.name == pn("narrowing")) p.hint = "The radial distance to use in all computations.";
        if (p.name == pn("rotation")) p.hint = "The rotation between both fisheyes.";
    }
    d.render = dualFishEye;
    d.changed = stitchChanged;
    out.push_back(d);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
