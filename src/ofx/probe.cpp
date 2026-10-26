#include "probe.h"
#include "ofxinternal.h"
#include <chrono>
#include <cstring>
#include <cmath>

namespace reno {
namespace ofx {

ImagePtr makeTestPattern(int w, int h, double frame)
{
    ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
    if (!img) return nullptr;
    static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {16, 16, 16}};
    const int bx = (int)std::fmod(frame * 7.0, (double)std::max(1, w - w / 6));
    const int by = h / 3;
    for (int y = 0; y < h; ++y) {
        uint8_t* d = img->row(y);
        for (int x = 0; x < w; ++x, d += 4) {
            uint8_t r, g, b;
            if (y > h / 2) {                               // top half: colour bars (row 0 is the bottom)
                const uint8_t* c = bars[std::min(7, x * 8 / w)];
                r = c[0]; g = c[1]; b = c[2];
            } else {                                       // bottom half: a gradient that drifts
                r = (uint8_t)((x * 255) / std::max(1, w - 1));
                g = (uint8_t)((y * 510) / std::max(1, h - 1));
                b = (uint8_t)(128 + 127 * std::sin((x + frame * 5.0) * 0.05));
            }
            if (x >= bx && x < bx + w / 6 && y >= by && y < by + h / 6) { r = 255; g = 128; b = 0; }
            d[0] = b; d[1] = g; d[2] = r; d[3] = 255;
        }
    }
    return img;
}

static uint64_t hashImage(const Image& img)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = img.data.get();
    for (size_t k = 0, n = img.byteSize(); k < n; k += 7) { h ^= p[k]; h *= 1099511628211ull; }
    return h;
}

// The context a plug-in is tried in: a filter if it can be one, else a generator, else a transition ("" for none).
static std::string pickContext(Plugin* pl)
{
    std::string context = pl->filterContext();
    if (context.empty() && pl->supportsContext(kOfxImageEffectContextGenerator)) context = kOfxImageEffectContextGenerator;
    if (context.empty() && pl->supportsContext(kOfxImageEffectContextTransition)) context = kOfxImageEffectContextTransition;
    return context;
}

// Settings that change nothing at their defaults (a brightness of 0, a blur of 0) would look like an effect that does
// nothing. Moves every number a third of the way to its largest shown value and renders one frame more.
static void nudgeSettingsAndRender(Instance& inst, const ProbeOptions& opt, Json& j)
{
    int moved = 0;
    for (const auto& p : inst.params.params) {
        if (p->dim <= 0 || p->isWhole() || p->secret()) continue;
        std::vector<double> v = p->value;
        for (int k = 0; k < p->dim; ++k) {
            const double hi = p->props.getDouble(kOfxParamPropDisplayMax, k, p->props.getDouble(kOfxParamPropMax, k, v[(size_t)k] + 1.0));
            const double lo = p->props.getDouble(kOfxParamPropDisplayMin, k, p->props.getDouble(kOfxParamPropMin, k, v[(size_t)k] - 1.0));
            const double was = v[(size_t)k];
            double to = was + (hi - was) / 3.0;
            if (!(to != was)) to = was + (lo - was) / 3.0;
            if (!std::isfinite(to) || std::fabs(to - was) > 1.0e4) to = was + std::max(1.0, std::fabs(was) * 0.25);
            if (std::isfinite(to)) v[(size_t)k] = to;
        }
        inst.userSetValue(p.get(), v, 10.0);
        ++moved;
    }
    if (!moved) return;
    RenderOptions ro;
    ro.interactive = false;
    std::string e2;
    ImagePtr out = inst.render(10.0, ro, &e2);
    ImagePtr src = makeTestPattern(opt.width, opt.height, 10.0);
    j["changedWhenNudged"] = out && hashImage(*out) != hashImage(*src);
    if (!out) j["nudgedError"] = e2;
}

// Uses the plug-in the way it would be used: makes an instance and renders opt.frames test frames through it, timing
// them. Fills in what happened in 'j'; returns false (with the reason in 'e') if it failed.
static bool probeRender(Plugin* pl, const ProbeOptions& opt, Json& j, std::string& e)
{
    const std::string context = pickContext(pl);
    if (context.empty()) {
        e = "has no way of being used that Reno knows (filter, generator, transition)";
        return false;
    }
    j["usedAs"] = context;
    InstanceSetup setup;
    setup.width = opt.width;
    setup.height = opt.height;
    setup.fps = 30.0;
    setup.durationFrames = 300;
    setup.vegasContext = context == kOfxImageEffectContextGenerator ? kVegasContextGenerator : kVegasContextEvent;
    std::unique_ptr<Instance> inst = pl->createInstance(context, setup, &e);
    if (!inst) return false;
    bool ok = true;
    int paramCount = 0;
    for (const auto& p : inst->params.params) if (p->hasValue()) ++paramCount;
    j["params"] = paramCount;
    for (const auto& name : inst->inputClipNames()) {
        const bool to = name == kOfxImageEffectTransitionSourceToClipName;
        inst->setClipSource(name, [opt, to](double t) { return makeTestPattern(opt.width, opt.height, to ? t + 40.0 : t); });
    }
    Param* progress = context == kOfxImageEffectContextTransition ? inst->params.find(kOfxImageEffectTransitionParamName) : nullptr;
    bool changed = false;
    double ms = 0;
    int done = 0;
    for (int f = 0; f < opt.frames && ok; ++f) {
        const double t = 10.0 + f;
        if (progress) progress->value[0] = (f + 1.0) / (opt.frames + 1.0);
        auto t0 = std::chrono::steady_clock::now();
        RenderOptions ro;
        ro.interactive = false;
        ImagePtr out = inst->render(t, ro, &e);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!out) { ok = false; break; }
        ++done;
        ImagePtr src = makeTestPattern(opt.width, opt.height, t);
        if (hashImage(*out) != hashImage(*src)) changed = true;
    }
    if (ok && done && !changed) nudgeSettingsAndRender(*inst, opt, j);
    j["rendered"] = done;
    j["changedPicture"] = changed;
    if (done) j["msPerFrame"] = ms / done;
    if (ok) { Json st = inst->saveState(); j["stateBytes"] = (int)st.dump().size(); }
    return ok;
}

// Only creates an instance (no rendering), to see that the plug-in can be made.
static bool probeInstance(Plugin* pl, const ProbeOptions& opt, Json& j, std::string& e)
{
    const std::string context = pickContext(pl);
    if (context.empty()) return true;
    InstanceSetup setup;
    setup.width = opt.width;
    setup.height = opt.height;
    setup.vegasContext = context == kOfxImageEffectContextGenerator ? kVegasContextGenerator : kVegasContextEvent;
    std::unique_ptr<Instance> inst = pl->createInstance(context, setup, &e);
    j["instanceMade"] = inst != nullptr;
    return inst != nullptr;
}

Json probeBinary(const std::string& path, const ProbeOptions& opt)
{
    Json rep = Json::object();
    rep["path"] = path;
    Host& host = Host::get();
    std::string err;
    Binary* bin = host.loadBinary(path, &err);
    if (!bin) {
        rep["ok"] = false;
        rep["error"] = err;
        return rep;
    }
    rep["ok"] = true;
    Json plugins = Json::array();
    for (auto& up : bin->plugins) {
        Plugin* pl = up.get();
        Json j = Json::object();
        j["id"] = pl->info.id;
        j["versionMajor"] = pl->info.versionMajor;
        j["versionMinor"] = pl->info.versionMinor;
        std::string e;
        bool ok = pl->load(&e);
        j["label"] = pl->info.label;
        j["group"] = pl->info.grouping;
        Json ctx = Json::array();
        for (const auto& c : pl->info.contexts) ctx.push(c);
        j["contexts"] = ctx;
        Json dep = Json::array();
        for (const auto& c : pl->info.depths) dep.push(c);
        j["depths"] = dep;
        j["ownWindow"] = pl->info.hasHwndInteract;
        j["overlay"] = pl->info.hasOverlay;
        if (ok && opt.render) ok = probeRender(pl, opt, j, e);
        else if (ok && opt.instance) ok = probeInstance(pl, opt, j, e);
        if (!pl->info.upliftGuids.empty()) {
            Json ug = Json::array();
            for (const auto& g : pl->info.upliftGuids) ug.push(g);
            j["replaces"] = ug;
        }
        j["ok"] = ok;
        if (!ok) j["error"] = e.empty() ? pl->error : e;
        j["crashed"] = pl->crashed;
        Json suites = Json::array();
        {
            std::lock_guard<std::mutex> lk(pl->suiteLogMutex);
            for (const auto& s : pl->suiteLog) suites.push(s);
        }
        j["suites"] = suites;
        Json missing = Json::array();
        {
            std::lock_guard<std::mutex> lk(pl->suiteLogMutex);
            for (const auto& s : pl->missingLog) missing.push(s);
        }
        j["propertiesNotThere"] = missing;
        if (!pl->missingFeature.empty()) j["missingFeature"] = pl->missingFeature;
        plugins.push(j);
    }
    rep["plugins"] = plugins;
    return rep;
}

}  // namespace ofx
}  // namespace reno
