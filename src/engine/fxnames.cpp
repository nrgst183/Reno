// The names effects are shown by (fxnames.h).
#include "fxnames.h"
#include "catalog.h"
#include "project.h"
#include "../ofx/builtins.h"
#include "../ofx/ofxhost.h"
#include <cctype>
#include <map>
#include <mutex>

namespace reno {

namespace {

// VEGAS Pro 18's own OpenFX effects and their labels.
const std::map<std::string, std::string>& vegasLabels()
{
    static const std::map<std::string, std::string> m = {
    {"com.magix.ofx.vr.dual.stich", u8"VEGAS Dual Fish Eye Stitching"},
    {"com.magix.ofx.vr.stabilization", u8"VEGAS 360\u00b0 Stabilization"},
    {"com.magixsoftware:panorotation", u8"VEGAS Scene Rotation"},
    {"com.vegascreativesoftware:3dblinds", u8"VEGAS 3D Blinds"},
    {"com.vegascreativesoftware:3dcascade", u8"VEGAS 3D Cascade"},
    {"com.vegascreativesoftware:3dflyinout", u8"VEGAS 3D Fly In/Out"},
    {"com.vegascreativesoftware:3dshuffle", u8"VEGAS 3D Shuffle"},
    {"com.vegascreativesoftware:addnoise", u8"VEGAS Add Noise"},
    {"com.vegascreativesoftware:autolooks", u8"VEGAS AutoLooks"},
    {"com.vegascreativesoftware:barndoor", u8"VEGAS Barn Door"},
    {"com.vegascreativesoftware:blackandwhite", u8"VEGAS Black and White"},
    {"com.vegascreativesoftware:blackbarfill", u8"VEGAS Black Bar Fill"},
    {"com.vegascreativesoftware:blackrestore", u8"VEGAS Black Restore"},
    {"com.vegascreativesoftware:border", u8"VEGAS Border"},
    {"com.vegascreativesoftware:brightnessandcontrast", u8"VEGAS Brightness and Contrast"},
    {"com.vegascreativesoftware:broadcastcolors", u8"VEGAS Broadcast Colors"},
    {"com.vegascreativesoftware:bumpmap", u8"VEGAS Bump Map"},
    {"com.vegascreativesoftware:bumpmapfilter", u8"VEGAS Bump Map"},
    {"com.vegascreativesoftware:bzmasking", u8"VEGAS B\u00e9zier Masking"},
    {"com.vegascreativesoftware:channelblend", u8"VEGAS Channel Blend"},
    {"com.vegascreativesoftware:checkerboard", u8"VEGAS Checkerboard"},
    {"com.vegascreativesoftware:chromablur", u8"VEGAS Chroma Blur"},
    {"com.vegascreativesoftware:chromakeyer", u8"VEGAS Chroma Keyer"},
    {"com.vegascreativesoftware:clockwipe", u8"VEGAS Clock Wipe"},
    {"com.vegascreativesoftware:colorbalance", u8"VEGAS Color Balance"},
    {"com.vegascreativesoftware:colorcorrector", u8"VEGAS Color Corrector"},
    {"com.vegascreativesoftware:colorcorrectorsecondary", u8"VEGAS Color Corrector (Secondary)"},
    {"com.vegascreativesoftware:colorcurves", u8"VEGAS Color Curves"},
    {"com.vegascreativesoftware:colorgradient", u8"VEGAS Color Gradient"},
    {"com.vegascreativesoftware:colorgrading", u8"VEGAS Color Grading"},
    {"com.vegascreativesoftware:colormatch", u8"VEGAS Color Match"},
    {"com.vegascreativesoftware:convolutionkernel", u8"VEGAS Convolution Kernel"},
    {"com.vegascreativesoftware:cookiecutter", u8"VEGAS Cookie Cutter"},
    {"com.vegascreativesoftware:crop", u8"VEGAS Crop"},
    {"com.vegascreativesoftware:crosseffect", u8"VEGAS Cross Effect"},
    {"com.vegascreativesoftware:defocusvelvetmatter", u8"VEGAS Defocus"},
    {"com.vegascreativesoftware:deform", u8"VEGAS Deform"},
    {"com.vegascreativesoftware:displacementmap", u8"VEGAS Displacement Map"},
    {"com.vegascreativesoftware:dissolve", u8"VEGAS Dissolve"},
    {"com.vegascreativesoftware:filllightvelvetmatter", u8"VEGAS Fill Light"},
    {"com.vegascreativesoftware:filmeffects", u8"VEGAS Film Effects"},
    {"com.vegascreativesoftware:filmgrain", u8"VEGAS Film Grain"},
    {"com.vegascreativesoftware:flash", u8"VEGAS Flash"},
    {"com.vegascreativesoftware:gaussianblur", u8"VEGAS Gaussian Blur"},
    {"com.vegascreativesoftware:glintvelvetmatter", u8"VEGAS Glint"},
    {"com.vegascreativesoftware:glow", u8"VEGAS Glow"},
    {"com.vegascreativesoftware:gradientmap", u8"VEGAS Gradient Map"},
    {"com.vegascreativesoftware:gradientwipe", u8"VEGAS Gradient Wipe"},
    {"com.vegascreativesoftware:heightmap", u8"VEGAS Height Map"},
    {"com.vegascreativesoftware:histogram", u8"VEGAS Histogram"},
    {"com.vegascreativesoftware:hsladjust", u8"VEGAS HSL Adjust"},
    {"com.vegascreativesoftware:invert", u8"VEGAS Invert"},
    {"com.vegascreativesoftware:iris", u8"VEGAS Iris"},
    {"com.vegascreativesoftware:labadjust", u8"VEGAS LAB Adjust"},
    {"com.vegascreativesoftware:layerdimensionality", u8"VEGAS Layer Dimensionality"},
    {"com.vegascreativesoftware:lensflare", u8"VEGAS Lens Flare"},
    {"com.vegascreativesoftware:levels", u8"VEGAS Levels"},
    {"com.vegascreativesoftware:lightrays", u8"VEGAS Light Rays"},
    {"com.vegascreativesoftware:linearblur", u8"VEGAS Linear Blur"},
    {"com.vegascreativesoftware:linearwipe", u8"VEGAS Linear Wipe"},
    {"com.vegascreativesoftware:lutfilter", u8"VEGAS LUT Filter"},
    {"com.vegascreativesoftware:maskgenerator", u8"VEGAS Mask Generator"},
    {"com.vegascreativesoftware:median", u8"VEGAS Median"},
    {"com.vegascreativesoftware:minandmax", u8"VEGAS Min and Max"},
    {"com.vegascreativesoftware:mirror", u8"VEGAS Mirror"},
    {"com.vegascreativesoftware:newsprint", u8"VEGAS Newsprint"},
    {"com.vegascreativesoftware:noisetexture", u8"VEGAS Noise Texture"},
    {"com.vegascreativesoftware:pageloop", u8"VEGAS Page Loop"},
    {"com.vegascreativesoftware:pagepeel", u8"VEGAS Page Peel"},
    {"com.vegascreativesoftware:pageroll", u8"VEGAS Page Roll"},
    {"com.vegascreativesoftware:pictureinpicture", u8"VEGAS Picture In Picture"},
    {"com.vegascreativesoftware:pinchpunch", u8"VEGAS Pinch/Punch"},
    {"com.vegascreativesoftware:pixelate", u8"VEGAS Pixelate"},
    {"com.vegascreativesoftware:portals", u8"VEGAS Portals"},
    {"com.vegascreativesoftware:push", u8"VEGAS Push"},
    {"com.vegascreativesoftware:quickblur", u8"VEGAS Quick Blur"},
    {"com.vegascreativesoftware:radialblur", u8"VEGAS Radial Blur"},
    {"com.vegascreativesoftware:radialpixelate", u8"VEGAS Radial Pixelate"},
    {"com.vegascreativesoftware:raysvelvetmatter", u8"VEGAS Rays"},
    {"com.vegascreativesoftware:saturationadjust", u8"VEGAS Saturation Adjust"},
    {"com.vegascreativesoftware:sepia", u8"VEGAS Sepia"},
    {"com.vegascreativesoftware:sharpen", u8"VEGAS Sharpen"},
    {"com.vegascreativesoftware:slide", u8"VEGAS Slide"},
    {"com.vegascreativesoftware:softcontrastvelvetmatter", u8"VEGAS Soft Contrast"},
    {"com.vegascreativesoftware:solidcolor", u8"VEGAS Solid Color"},
    {"com.vegascreativesoftware:spherize", u8"VEGAS Spherize"},
    {"com.vegascreativesoftware:spiral", u8"VEGAS Spiral"},
    {"com.vegascreativesoftware:split", u8"VEGAS Split"},
    {"com.vegascreativesoftware:squeeze", u8"VEGAS Squeeze"},
    {"com.vegascreativesoftware:starburstvelvetmatter", u8"VEGAS Starburst"},
    {"com.vegascreativesoftware:starwipe", u8"VEGAS Star Wipe"},
    {"com.vegascreativesoftware:swap", u8"VEGAS Swap"},
    {"com.vegascreativesoftware:swirl", u8"VEGAS Swirl"},
    {"com.vegascreativesoftware:testpattern", u8"VEGAS Test Pattern"},
    {"com.vegascreativesoftware:threshold", u8"VEGAS Threshold"},
    {"com.vegascreativesoftware:timecode", u8"VEGAS Timecode"},
    {"com.vegascreativesoftware:titlesandtext", u8"VEGAS Titles & Text"},
    {"com.vegascreativesoftware:creditroll", u8"VEGAS Credit Roll"},
    {"com.vegascreativesoftware:tvsimulator", u8"VEGAS TV Simulator"},
    {"com.vegascreativesoftware:unsharpmask", u8"VEGAS Unsharp Mask"},
    {"com.vegascreativesoftware:venetianblinds", u8"VEGAS Venetian Blinds"},
    {"com.vegascreativesoftware:vignette", u8"VEGAS Vignette"},
    {"com.vegascreativesoftware:wave", u8"VEGAS Wave"},
    {"com.vegascreativesoftware:whitebalance", u8"VEGAS White Balance"},
    {"com.vegascreativesoftware:zoom", u8"VEGAS Zoom"},
    {"de.magix:stereographicProjectionOCL", u8"VEGAS Tiny Planet"},
    {"de.magix:MeshWarp", u8"VEGAS Mesh Warp"},
    {"com.sonycreativesoftware:smart_upscale", u8"Sony Smart Upscale"},
    {"com.vegascreativesoftware:Stabilize", u8"VEGAS Stabilize"},
    {"de.magix:Stabilize", u8"VEGAS Video Stabilization"},
    {"de.magix:MotionTracker", u8"VEGAS Motion Tracker"},
    {"de.magix:FlickerReducer", u8"VEGAS Flicker Control"},
    {"de.magix:DenoisingNLM", u8"VEGAS Denoise"},
    {"de.magix:Colorization", u8"VEGAS Colorization"},
    {"de.magix:StyleTransfer", u8"VEGAS Style Transfer"},
    {"com.sonycreativesoftware:smart_noise_reduction", u8"Sony Smart Noise Reduction"},
    };
    return m;
}

// "VEGAS Gaussian Blur" for a plug-in that called itself by its identifier: no label of its own.
bool isRealLabel(const std::string& label, const std::string& id)
{
    return !label.empty() && label != id;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

}  // namespace

std::string vegasStockFxLabel(const std::string& pluginId)
{
    auto it = vegasLabels().find(pluginId);
    return it == vegasLabels().end() ? std::string() : it->second;
}

std::string readableFxId(const std::string& pluginId)
{
    // the part after the last ':' and then after the last '.'.
    std::string id = trim(pluginId);
    if (id.empty()) return id;
    std::string tail = id.substr(id.find_last_of(':') == std::string::npos ? 0 : id.find_last_of(':') + 1);
    if (tail.empty()) tail = id.substr(0, id.size() - 1);
    if (tail.empty()) return id;
    std::string word = tail;
    const size_t dot = tail.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < tail.size()) {
        word = tail.substr(dot + 1);
        bool digits = true;
        for (char c : word) if (!std::isdigit((unsigned char)c)) digits = false;
        if (digits && dot > 0) {
            const size_t before = tail.find_last_of('.', dot - 1);
            word = tail.substr(before == std::string::npos ? 0 : before + 1, dot - (before == std::string::npos ? 0 : before + 1)) + " " + word;
        }
    }
    // words: '_' and '-' are spaces; a capital after a small letter or a digit, a digit after a letter, and the last
    // capital of a run of capitals followed by a small letter.
    std::string out;
    for (size_t k = 0; k < word.size(); ++k) {
        const char c = word[k];
        if (c == '_' || c == '-') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        if (!out.empty() && out.back() != ' ') {
            const char p = word[k - 1];
            const bool up = std::isupper((unsigned char)c) != 0, digit = std::isdigit((unsigned char)c) != 0;
            const bool pLow = std::islower((unsigned char)p) != 0, pUp = std::isupper((unsigned char)p) != 0, pDigit = std::isdigit((unsigned char)p) != 0;
            const bool nextLow = k + 1 < word.size() && std::islower((unsigned char)word[k + 1]);
            if ((up && (pLow || pDigit)) || (digit && (pLow || pUp)) || (up && pUp && nextLow)) {
                if (!(up && pDigit && (k + 1 >= word.size() || !std::islower((unsigned char)word[k + 1])))) out += ' ';
            }
        }
        out += c;
    }
    out = trim(out);
    if (out.empty()) return id;
    if (std::islower((unsigned char)out[0])) out[0] = (char)std::toupper((unsigned char)out[0]);
    return out;
}

std::string videoFxLabel(const std::string& pluginId)
{
    if (pluginId.empty()) return std::string();
    // (Reno's built-in stand-in for one of VEGAS's own effects is shown by VEGAS's label: "VEGAS Cookie Cutter", not
    // the stand-in's short "Cookie Cutter", wherever its label is found)
    const std::string stockLabel = vegasStockFxLabel(pluginId);
    std::string builtinLabel;
    for (const auto& b : ofx::builtinInfos())
        if (b.id == pluginId) builtinLabel = b.label;
    auto shown = [&](const std::string& label) { return !stockLabel.empty() && label == builtinLabel ? stockLabel : label; };
    // the plug-in list: what the plug-in called itself when it was tried
    VideoFxEntry e;
    if (Catalog::get().findVideo(pluginId, &e) && isRealLabel(e.label, pluginId)) return shown(e.label);
    // a plug-in loaded here (not in the list: OFX_PLUGIN_PATH, a project's own folder...) once it has described
    // itself.
    for (ofx::Plugin* pl : ofx::Host::get().allPlugins()) {
        if (!pl || pl->info.id != pluginId) continue;
        std::unique_lock<std::recursive_mutex> lk(pl->loadMutex, std::try_to_lock);
        if (lk.owns_lock() && pl->loaded && isRealLabel(pl->info.label, pluginId)) return shown(pl->info.label);
    }
    // VEGAS's own effect of that identifier, or Reno's built-in stand-in
    if (!stockLabel.empty()) return stockLabel;
    for (const auto& b : ofx::builtinInfos())
        if (b.id == pluginId && isRealLabel(b.label, pluginId)) return b.label;
    return readableFxId(pluginId);
}

std::string fxSlotLabel(const FxSlot& slot)
{
    if (slot.kind == "audio") {
        if (!slot.name.empty()) return slot.name;
        if (!slot.audio.name.empty()) return slot.audio.name;
        if (!slot.audio.path.empty()) {
            std::string f = pathFileName(slot.audio.path);
            const size_t dot = f.find_last_of('.');
            return dot != std::string::npos && dot > 0 ? f.substr(0, dot) : f;
        }
        return slot.audio.uid.empty() ? std::string("Audio effect") : slot.audio.uid;
    }
    if (isRealLabel(slot.name, slot.pluginId)) return slot.name;
    if (!slot.pluginId.empty()) return videoFxLabel(slot.pluginId);
    return slot.name;
}

}  // namespace reno
