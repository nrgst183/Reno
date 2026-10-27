// Built-in stand-in for VEGAS Pro 18's Dual Fish Eye Stitching (manual: "360° video", "Stitching"): the frame of a 360° camera with two fish-eye lenses side by side (front lens's circle
// in the left half, back lens's in the right) made into one equirectangular 360° frame, the seam blended and the vignetting towards each circle's edge removed. Uses VEGAS's identifier and
// settings (from its OfxStitch bundle: gui.xml gives ranges and defaults, VrPresets.xml the camera models). The picture follows the plug-in's OpenCL kernel (what VEGAS draws on the GPU).
#include "builtins_impl.h"
#include "vkfilters.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

const char* const kPre = "mx.ofx.360.stitch.fisheye.";
std::string pn(const char* n) { return std::string(kPre) + n; }

const float kPif = 3.1415927410125732f, kHalfPif = 1.5707963705062866f;

// VEGAS draws this effect with the 360 plug-in's OpenCL kernel (VrDualStitch.dll carries its source, every byte with the high bit set; the kernel is "rotateEquirect" with the fisheye
// branch retrievePixelInFishEye); this is that kernel in float, line by line, with what the GPU adds to it (found on VEGAS's pictures, as for Scene Rotation):
//  - a pixel (x, y) of the W x H picture is at longitude (2 x / (W - 1) - 1) pi and latitude (1 - 2 y / (H - 1)) pi / 2; the GPU's native cos(pi / 2) is +0, sin(pi) +3.8e-7, sin(-pi) -0;
//  - the lenses sit in the picture's top W / 2 rows (the kernel maps with height = width / 2 and flips the rows with the frame height), the sampler is linear with unnormalized
//    coordinates (the texel is at the position - 0.5), clamped at the edges, with weights in whole 1/256s; the result is cut to 12 bits and read as (r12 * 255 + 2047) / 4096 levels;
//  - the vignetting is removed on the float colours (gamma = 1 - factor t t), the overlap of the lenses blended in YUV.
struct KSettings {
    float aperture = 0, factor = 0, range = 0, narrowOff = 0, narrowRad = 1, eyeRotate = 0;
    int orient[2] = {0, 0};                                         // [0] = x (the back lens), [1] = y (the front lens)
};

struct KVec2 { float x, y; };

KVec2 rotateAbout90(KVec2 n, int orientation, float angle)
{
    switch (orientation) {
        case 1: angle += 1.5707963f; break;
        case 2: angle += kPif; break;
        case 3: angle -= 1.5707963f; break;
    }
    const float rs = (float)std::sin((double)angle), rc = (float)std::cos((double)angle);
    KVec2 r;
    r.x = n.x * rc - n.y * rs;
    r.y = n.x * rs + n.y * rc;
    return r;
}

KVec2 fishEyePoint(float sx, float sy, float sz, const KSettings& k, bool useFront, float& radius)
{
    float faRad = 2.0f * atan2f(hypotf(sz, sy), sx) / k.aperture;
    const float offsetRad = faRad * k.narrowOff;
    faRad -= offsetRad;
    faRad *= k.narrowRad;
    radius = std::fabs(faRad);
    const float faAngle = atan2f(sy, sz);
    KVec2 n;
    n.x = faRad * (float)std::cos((double)faAngle);
    n.y = faRad * (float)std::sin((double)faAngle);
    const float angle = k.eyeRotate * kPif / 180.0f;
    if (useFront) {
        n = rotateAbout90(n, k.orient[1], angle);
        n.x += 1.0f;
        n.x += offsetRad;
    } else {
        n = rotateAbout90(n, k.orient[0], angle);
        n.x -= 1.0f;
        n.x -= offsetRad;
    }
    n.x /= 2.0f;
    return n;
}

struct KPicture {
    const Buf* src = nullptr;
    int W = 0, H = 0;
    bool levels = false;
};

// The GPU's result: cut to 12 bits, read as (r12 * 255 + 2047) / 4096 levels.
float quant12(float r)
{
    const long long r12 = (long long)std::floor(std::min(std::max(r, 0.0f), 1.0f) * 4096.0f);
    return (float)((r12 * 255 + 2047) / 4096) / 255.0f;
}

// Where one lens's colour comes from: the texel (x0, y0) (memory rows, from the bottom) and its neighbours with the weights fx, fy, and the vignetting exponent (kNoGamma: none).
const float kNoGamma = 1e30f;
struct KSample {
    int x0, y0;
    float fx, fy, gamma;
};

KSample makeSample(const KPicture& pic, KVec2 norm, float radius, const KSettings& k)
{
    const int W = pic.W, H = pic.H;
    const float ww = (float)(W - 1), hh = (float)(W / 2 - 1);
    const float mx = norm.x > -1.f ? (1.f > norm.x ? norm.x : 1.f) : -1.f, my = norm.y > -1.f ? (1.f > norm.y ? norm.y : 1.f) : -1.f;
    const float px = (mx + 1.0f) / 2.0f * ww;
    float py = hh - (my + 1.0f) / 2.0f * hh;
    py = (float)(H - 1) - py;                                       // the image has its first row at the bottom
    const float sx = px - 0.5f, sy = py - 0.5f;
    const float fxs = std::floor(sx), fys = std::floor(sy);
    KSample r;
    r.x0 = (int)fxs;
    r.y0 = (int)fys;
    r.fx = sx - fxs;
    r.fy = sy - fys;
    if (pic.levels) {
        r.fx = std::floor(r.fx * 256.0f + 0.5f) / 256.0f;
        r.fy = std::floor(r.fy * 256.0f + 0.5f) / 256.0f;
    }
    r.gamma = kNoGamma;
    if (k.factor > 0.0f && k.range != 0.0f) {
        const float fallOffMin = 1.0f - k.range;
        if (!(radius < fallOffMin)) {
            float rr = radius - fallOffMin;
            rr /= k.range;
            r.gamma = 1.0f - (k.factor * rr * rr);
        }
    }
    return r;
}

void shadeSample(const KPicture& pic, const KSample& s, float out[3])
{
    const int W = pic.W, H = pic.H;
    auto cl = [](int i, int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); };
    // memory row m (from the bottom) is row H - 1 - m of the buffer
    const float* t00 = pic.src->at(cl(s.x0, W), H - 1 - cl(s.y0, H));
    const float* t10 = pic.src->at(cl(s.x0 + 1, W), H - 1 - cl(s.y0, H));
    const float* t01 = pic.src->at(cl(s.x0, W), H - 1 - cl(s.y0 + 1, H));
    const float* t11 = pic.src->at(cl(s.x0 + 1, W), H - 1 - cl(s.y0 + 1, H));
    for (int c = 0; c < 3; ++c) {
        const float top = t00[c] * (1.0f - s.fx) + t10[c] * s.fx, bot = t01[c] * (1.0f - s.fx) + t11[c] * s.fx;
        out[c] = top * (1.0f - s.fy) + bot * s.fy;
    }
    if (s.gamma != kNoGamma) {
        for (int c = 0; c < 3; ++c) {
            const float q = out[c] > 0.f ? powf(out[c], s.gamma) : 0.f;
            out[c] = q < 0.f ? 0.f : (q > 1.f ? 1.f : q);
        }
    }
}

void yuvOf(const float c[3], float y[3])
{
    y[0] = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    y[1] = 0.436f * ((c[2] - y[0]) / (1.0f - 0.114f));
    y[2] = 0.615f * ((c[0] - y[0]) / (1.0f - 0.299f));
}

// What a pixel of the stitched picture takes: one lens (blend false, s[0]) or both blended in YUV (distK is the front lens's weight).
struct KPix {
    bool blend;
    float distK;
    KSample s[2];
};

struct KStitch {
    KSettings k;
    KPicture pic;
    float fovDist = 0;
    std::vector<float> cosL, sinL;
};

bool stitchSetup(Ctx& c, int W, int H, KStitch& st)
{
    if (W < 2 || H < 2) return false;
    KSettings& k = st.k;
    const float fov = ((float)c.d(pn("fov").c_str()) * kPif) / 180.f;
    if (!(fov > 0.f)) return false;
    k.aperture = fov;
    k.factor = c.b(pn("falloff.use").c_str()) ? (float)(c.d(pn("falloff.factor").c_str()) / 100.0) : 0.f;
    k.range = (float)(c.d(pn("falloff.range").c_str()) / 100.0);
    k.narrowOff = (float)(c.d(pn("center").c_str()) / 100.0);
    k.narrowRad = (float)(c.d(pn("narrowing").c_str()) / 100.0);
    k.eyeRotate = (float)(c.d(pn("rotation").c_str()) / 100.0);
    // the orientations as VEGAS numbers them inside: 0, 90, 180, -90 degrees for the entries 0 to 3 of the list
    static const int kMap[4] = {0, 3, 2, 1};
    const int oL = kMap[std::min(3, std::max(0, c.i(pn("orient.left").c_str())))];
    const int oR = kMap[std::min(3, std::max(0, c.i(pn("orient.right").c_str())))];
    k.orient[0] = oL;
    k.orient[1] = oR;
    st.pic.src = &c.src;
    st.pic.W = W;
    st.pic.H = H;
    st.pic.levels = c.levels != 0;
    st.fovDist = (float)std::fabs((double)std::sin((double)((fov - kPif) / 2.0f)));
    // the angles of the columns
    st.cosL.resize((size_t)W);
    st.sinL.resize((size_t)W);
    for (int ix = 0; ix < W; ++ix) {
        const float lon = ((float)ix / (float)(W - 1) * 2.0f - 1.0f) * kPif;
        st.cosL[(size_t)ix] = (float)std::cos((double)lon);
        float sl = (float)std::sin((double)lon);
        if (lon >= kPif) sl = 3.8e-7f;
        else if (lon <= -kPif) sl = -0.0f;
        st.sinL[(size_t)ix] = sl;
    }
    return true;
}

// The rows' latitude: (cos, sin)
void stitchRow(const KStitch& st, int y, float& cp, float& sp)
{
    const int H = st.pic.H;
    const float hh = (float)(H - 1);
    const float latR = (((float)(H - 1) - (float)y) / hh * 2.0f - 1.0f) * kHalfPif;
    cp = (float)std::cos((double)latR);
    if (std::fabs(latR) >= kHalfPif) cp = 0.0f;
    sp = (float)std::sin((double)latR);
}

KPix stitchPixel(const KStitch& st, int x, float cp, float sp)
{
    KPix px;
    float sX = cp * st.cosL[(size_t)x], sY = sp, sZ = cp * st.sinL[(size_t)x];
    const float fovDist = st.fovDist;
    if (std::fabs(sX) < fovDist) {
        const float realX = sX;
        float distK = std::fabs(sX) / fovDist;
        float rF, rB;
        const KVec2 nF = fishEyePoint(sX, sY, sZ, st.k, true, rF);
        const KVec2 nB = fishEyePoint(-sX, sY, -sZ, st.k, false, rB);
        if (realX < 0.0f) { distK = 1.0f - distK; distK *= 0.5f; }
        else if (realX > 0.0f) { distK += 1.0f; distK /= 2.0f; }
        else distK = 0.5f;
        px.blend = true;
        px.distK = distK;
        px.s[0] = makeSample(st.pic, nF, rF, st.k);
        px.s[1] = makeSample(st.pic, nB, rB, st.k);
    } else {
        bool front = true;
        if (sX <= -fovDist) { front = false; sX = std::fabs(sX); sZ *= -1.0f; }
        float rad;
        const KVec2 n = fishEyePoint(sX, sY, sZ, st.k, front, rad);
        px.blend = false;
        px.distK = 0;
        px.s[0] = makeSample(st.pic, n, rad, st.k);
        px.s[1] = px.s[0];
    }
    return px;
}

void stitchShade(const KStitch& st, const KPix& px, float col[3])
{
    if (px.blend) {
        float cf[3], cb[3], yf[3], yb[3];
        shadeSample(st.pic, px.s[0], cf);
        shadeSample(st.pic, px.s[1], cb);
        yuvOf(cf, yf);
        yuvOf(cb, yb);
        const float distK = px.distK, backDistance = 1.0f - distK;
        const float Y = yf[0] * distK + yb[0] * backDistance, U = yf[1] * distK + yb[1] * backDistance, V = yf[2] * distK + yb[2] * backDistance;
        col[0] = std::min(1.f, std::max(0.f, Y + 1.14f * V));
        col[1] = std::min(1.f, std::max(0.f, Y - 0.395f * U - 0.581f * V));
        col[2] = std::min(1.f, std::max(0.f, Y + 2.033f * U));
    } else {
        shadeSample(st.pic, px.s[0], col);
    }
}

void dualFishEye(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    KStitch st;
    if (c.src.w != W || c.src.h != H || !stitchSetup(c, W, H, st)) { c.out.px = c.src.px; return; }
    forRows(H, W, [&](int y) {
        float cp, sp;
        stitchRow(st, y, cp, sp);
        for (int x = 0; x < W; ++x) {
            float col[3];
            stitchShade(st, stitchPixel(st, x, cp, sp), col);
            float* o = c.out.at(x, y);
            for (int q = 0; q < 3; ++q) o[q] = st.pic.levels ? quant12(col[q]) : col[q];
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

// For the GPU (vkfilters.cpp): the sources of every pixel of the stitched picture, 12 floats each: mode (0 one lens, 1 both blended), the front lens's weight, then for each
// of the two lenses x0, y0, fx, fy, gamma (kNoGamma: none). Returns false where the effect copies the picture. The placement does not depend on the picture's colours.
bool stitchMapGpu(Ctx& c, int W, int H, std::vector<float>& out, bool& levels, float& noGamma)
{
    KStitch st;
    if (!stitchSetup(c, W, H, st)) return false;
    levels = st.pic.levels;
    noGamma = kNoGamma;
    // the map is kept while the settings stay (the planner runs for every frame)
    static std::mutex m;
    static std::string cachedKey;
    static std::vector<float> cached;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%dx%d;%d;%a,%a,%a,%a,%a,%a,%d,%d", W, H, levels ? 1 : 0, st.k.aperture, st.k.factor, st.k.range, st.k.narrowOff, st.k.narrowRad, st.k.eyeRotate, st.k.orient[0], st.k.orient[1]);
    std::lock_guard<std::mutex> lock(m);
    if (cachedKey != buf || cached.size() != (size_t)W * H * 12) {
        cachedKey.clear();
        cached.assign((size_t)W * H * 12, 0.f);
        for (int y = 0; y < H; ++y) {
            float cp, sp;
            stitchRow(st, y, cp, sp);
            for (int x = 0; x < W; ++x) {
                const KPix px = stitchPixel(st, x, cp, sp);
                float* r = &cached[((size_t)y * W + x) * 12];
                r[0] = px.blend ? 1.f : 0.f;
                r[1] = px.distK;
                for (int l = 0; l < 2; ++l) {
                    r[2 + l * 5] = (float)px.s[l].x0;
                    r[3 + l * 5] = (float)px.s[l].y0;
                    r[4 + l * 5] = px.s[l].fx;
                    r[5 + l * 5] = px.s[l].fy;
                    r[6 + l * 5] = px.s[l].gamma;
                }
            }
        }
        cachedKey = buf;
    }
    out = cached;
    return true;
}

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
