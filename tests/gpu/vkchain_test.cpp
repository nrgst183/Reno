// Tests the Vulkan path through the real engine: a still with a chain of built-in effects on its event is rendered once
// on the CPU (GPU API off) and once on Vulkan. The pictures must agree within a level or two, the CPU render must be
// unchanged by the GPU code, and the frames between consecutive effects must stay on the card (the counters say how
// many runs found their input already there). Skips with a message when there is no Vulkan driver; set
// VK_ICD_FILENAMES to SwiftShader's json and RENO_VK_ALLOW_SOFTWARE=1 to run it on a CPU implementation.
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/ofxhost.h"
#include "../../src/ofx/vkgpu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A picture with gradients, hard edges and a soft alpha ramp ("pattern:" - any other path is not opened).
class Pattern : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        ImagePtr img = Image::make(std::min(maxW, mi.width), std::min(maxH, mi.height), PixelDepth::Byte, false);
        for (int y = 0; y < img->h; ++y) {
            uint8_t* d = img->row(img->h - 1 - y);
            for (int x = 0; x < img->w; ++x, d += 4) {
                int r = (x * 255) / std::max(1, img->w - 1), g = (y * 255) / std::max(1, img->h - 1), b = 255 - r / 2 - g / 2;
                if (((x / 12) + (y / 12)) % 5 == 0) { r = 250; g = 20; b = 20; }
                const int a = y > img->h * 3 / 4 ? 255 - (y - img->h * 3 / 4) * 8 : 255;
                d[0] = (uint8_t)(b * a / 255); d[1] = (uint8_t)(g * a / 255); d[2] = (uint8_t)(r * a / 255); d[3] = (uint8_t)std::max(0, a);
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};
static std::unique_ptr<MediaReader> openPattern(const std::string& path, std::string* err)
{
    if (path != "pattern:") { if (err) *err = "unknown"; return nullptr; }
    std::unique_ptr<Pattern> r(new Pattern());
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = 192;
    r->mi.height = 108;
    return std::unique_ptr<MediaReader>(r.release());
}

static FxSlot slot(Project& p, const std::string& id, const Json& params = Json())
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = id;
    s.name = id;
    if (params.isObject()) {
        Json st = Json::object();
        st["params"] = params;
        s.state = st;
    }
    return s;
}
static Json val(std::initializer_list<double> v)
{
    Json e = Json::object(), a = Json::array();
    for (double x : v) a.push(x);
    e["v"] = a;
    return e;
}
static const std::string kV = "com.vegascreativesoftware:";

struct Rig {
    MediaPool pool;
    FxRuntime fx;
    VideoEngine video{pool, fx};
    FrameOptions fo;
    Rig() { pool.setOpener(openPattern); }
    ~Rig() { fx.releaseExcept({}); }
    Project chain(const std::vector<std::pair<std::string, Json>>& effects, bool onTrack = false)
    {
        Project p;
        p.settings.width = 192;
        p.settings.height = 108;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        p.tracks.push_back(t);
        MediaItem mi;
        mi.id = p.newId();
        mi.path = "pattern:";
        std::string err;
        mi.info = openPattern(mi.path, &err)->info();
        p.media.push_back(mi);
        Event e;
        e.id = p.newId();
        e.mediaId = mi.id;
        e.length = 5;
        for (const auto& f : effects) {
            if (onTrack) p.tracks[0].fx.push_back(slot(p, kV + f.first, f.second));
            else e.fx.push_back(slot(p, kV + f.first, f.second));
        }
        p.tracks[0].events.push_back(e);
        return p;
    }
    ImagePtr render(const Project& p, double t = 1.0)
    {
        fx.clear();
        pool.clear();
        fx.prepare(p);
        return video.render(p, t, fo);
    }
};

static int worst(const ImagePtr& a, const ImagePtr& b, double* mean)
{
    int w = 0;
    double sum = 0;
    size_t n = 0;
    for (int y = 0; y < a->h; ++y) {
        const uint8_t* p = a->row(y);
        const uint8_t* q = b->row(y);
        for (int x = 0; x < a->w * 4; ++x, ++n) {
            const int d = std::abs((int)p[x] - (int)q[x]);
            sum += d;
            w = std::max(w, d);
        }
    }
    *mean = n ? sum / (double)n : 0;
    return w;
}

int main()
{
    std::string why;
    gpu::setEnabled(true);
    if (!gpu::ready(&why)) {
        std::printf("vkchain_test: SKIPPED - %s\n", why.c_str());
        return 0;
    }
    std::printf("using %s\n", gpu::status().c_str());
    Rig rig;

    struct Case { const char* name; std::vector<std::pair<std::string, Json>> fx; bool onTrack; };
    Json bc = Json::object();
    bc["Brightness"] = val({0.1});
    bc["Contrast"] = val({0.3});
    Json hue = Json::object();
    hue["AddToHue"] = val({60});
    Json blur = Json::object();
    blur["HorizontalRange"] = val({0.4});
    blur["VerticalRange"] = val({0.2});
    Json mir = Json::object();
    mir["Angle"] = val({20});
    Json ccs = Json::object();
    ccs["RotateHue"] = val({100});
    const Case cases[] = {
        {"one effect (Invert)", {{"invert", Json()}}, false},
        {"two effects (Brightness and Contrast, HSL Adjust)", {{"brightnessandcontrast", bc}, {"hsladjust", hue}}, false},
        {"four effects (brightness, hue, blur, sharpen)", {{"brightnessandcontrast", bc}, {"hsladjust", hue}, {"gaussianblur", blur}, {"sharpen", Json()}}, false},
        {"a chain with a mirror and Color Corrector (Secondary)", {{"mirror", mir}, {"colorcorrectorsecondary", ccs}, {"invert", Json()}}, false},
        {"the same on a track", {{"brightnessandcontrast", bc}, {"hsladjust", hue}, {"sharpen", Json()}}, true},
    };
    for (const Case& cs : cases) {
        const Project p = rig.chain(cs.fx, cs.onTrack);
        gpu::setEnabled(false);
        ImagePtr cpu = rig.render(p);
        ImagePtr cpu2 = rig.render(p);
        gpu::setEnabled(true);
        gpu::dropResident();
        const gpu::Counters before = gpu::counters();
        ImagePtr gp = rig.render(p);
        const gpu::Counters after = gpu::counters();
        const uint64_t runs = after.runs - before.runs, stayed = after.inputsStayed - before.inputsStayed, deferred = after.deferred - before.deferred;
        // the same chain with every frame downloaded between the effects (no deferral): the pictures must be the very same bytes
        gpu::setChainDeferral(false);
        gpu::dropResident();
        ImagePtr gp2 = rig.render(p);
        gpu::setChainDeferral(true);
        const std::string n = cs.name;
        check(cpu && cpu2 && gp && cpu->w == gp->w && cpu->h == gp->h, n + ": rendered");
        if (!cpu || !gp || cpu->w != gp->w || cpu->h != gp->h) continue;
        check(std::memcmp(cpu->row(0), cpu2->row(0), (size_t)cpu->w * 4 * cpu->h) == 0, n + ": the CPU render is repeatable");
        double mean = 0;
        const int w = worst(cpu, gp, &mean);
        check(w <= 3, n + ": Vulkan picture within 3 levels of the CPU's", strfmt("worst %d, mean %.4f", w, mean));
        check(runs == (uint64_t)cs.fx.size(), n + ": every effect ran on the card", strfmt("%d runs for %d effects", (int)runs, (int)cs.fx.size()));
        check(gp2 && gp2->w == gp->w && gp2->h == gp->h && std::memcmp(gp->row(0), gp2->row(0), (size_t)gp->w * 4 * gp->h) == 0, n + ": chained without downloads == downloaded one by one, byte for byte");
        check(deferred + 1 == (uint64_t)cs.fx.size() || cs.fx.size() < 2, n + ": downloads left out between the effects", strfmt("%d deferred for %d effects", (int)deferred, (int)cs.fx.size()));
        check(cs.fx.size() < 2 || stayed + 1 == (uint64_t)cs.fx.size(), n + ": the frames stayed on the card between effects", strfmt("%d inputs stayed of %d runs", (int)stayed, (int)runs));
    }

    // a chain with a bypassed effect in the middle, an effect that is not a built-in one with kernels, and a pan/crop-free event with a mask: the frame left on the card
    // by an effect must reach the next one that runs, or come down for whatever else reads it
    {
        Project p = rig.chain({{"invert", Json()}, {"hsladjust", hue}, {"sharpen", Json()}, {"brightnessandcontrast", bc}}, false);
        p.tracks[0].events[0].fx[1].bypass = true;
        gpu::setEnabled(false);
        ImagePtr cpu = rig.render(p);
        gpu::setEnabled(true);
        gpu::dropResident();
        ImagePtr gp = rig.render(p);
        double mean = 0;
        check(cpu && gp && worst(cpu, gp, &mean) <= 3, "a bypassed effect in the middle of a chain", strfmt("mean %.4f", mean));
        Project q = rig.chain({{"invert", Json()}, {"sharpen", Json()}}, false);
        q.tracks[0].events[0].fx.push_back(slot(q, "com.vegascreativesoftware:nosucheffect"));
        q.tracks[0].events[0].fx.push_back(slot(q, kV + "invert"));
        gpu::setEnabled(false);
        cpu = rig.render(q);
        gpu::setEnabled(true);
        gpu::dropResident();
        gp = rig.render(q);
        check(cpu && gp && worst(cpu, gp, &mean) <= 3, "an effect that cannot run in the middle of a chain", strfmt("mean %.4f", mean));
    }

    // GPU off: nothing runs on the card
    {
        const Project p = rig.chain(cases[2].fx, false);
        gpu::setEnabled(false);
        const gpu::Counters before = gpu::counters();
        rig.render(p);
        const gpu::Counters after = gpu::counters();
        check(after.runs == before.runs && after.failed == before.failed, "GPU API off: no work reaches the card");
    }
    gpu::shutdown();
    std::printf("vkchain_test: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
