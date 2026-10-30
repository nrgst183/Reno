// The host itself: finding and loading plug-ins, describing them, instances, rendering.
#include "ofxinternal.h"
#include "builtins.h"
#include "gpurender.h"
#include "vkgpu.h"
#include "../engine/profile.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace reno {
namespace ofx {

const char* depthString(PixelDepth d) { return d == PixelDepth::Float ? kOfxBitDepthFloat : kOfxBitDepthByte; }

static const char* pixelOrder(PixelDepth d) { return d == PixelDepth::Byte ? kVegasPixelOrderBGRA : kVegasPixelOrderRGBA; }

void fillImageProps(PropertySet& s, const Image& img, const std::string& id, const double scale[2])
{
    s.setString(kOfxPropType, kOfxTypeImage);
    s.setString(kOfxImageEffectPropPixelDepth, depthString(img.depth));
    s.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    s.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    s.setDouble(kOfxImageEffectPropRenderScale, scale ? scale[0] : 1.0, 0);
    s.setDouble(kOfxImageEffectPropRenderScale, scale ? scale[1] : 1.0, 1);
    s.setDouble(kOfxImagePropPixelAspectRatio, 1.0);
    s.setPointer(kOfxImagePropData, (void*)img.data.get());
    const int b[4] = {0, 0, img.w, img.h};
    for (int k = 0; k < 4; ++k) {
        s.setInt(kOfxImagePropBounds, b[k], k);
        s.setInt(kOfxImagePropRegionOfDefinition, b[k], k);
    }
    s.setInt(kOfxImagePropRowBytes, (int)img.rowBytes);
    s.setString(kOfxImagePropField, kOfxImageFieldNone);
    s.setString(kOfxImagePropUniqueIdentifier, id);
    s.setDouble("RenoImageSerial", (double)img.serial);
    if (Host::get().config.vegasExtensions) s.setString(kVegasPropPixelOrder, pixelOrder(img.depth));
}

// ====================================================================== Host
Host& Host::get()
{
    static Host* h = new Host();
    return *h;
}

Host::Host()
{
    ofxHost.host = (OfxPropertySetHandle)&hostProps;
    ofxHost.fetchSuite = fetchSuiteImpl;
    applyConfig();
}

void Host::applyConfig()
{
    PropertySet& s = hostProps;
    s.setString(kOfxPropType, kOfxTypeImageEffectHost);
    s.setString(kOfxPropName, config.hostName);
    s.setString(kOfxPropLabel, config.hostLabel);
    s.setInt(kOfxPropVersion, config.versionMajor, 0);
    s.setInt(kOfxPropVersion, config.versionMinor, 1);
    s.setInt(kOfxPropVersion, 0, 2);
    s.setString(kOfxPropVersionLabel, strfmt("%d.%d", config.versionMajor, config.versionMinor));
    s.setInt(kOfxPropAPIVersion, 1, 0);
    s.setInt(kOfxPropAPIVersion, 4, 1);
    s.setInt(kOfxImageEffectHostPropIsBackground, 0);
    s.setInt(kOfxImageEffectPropSupportsOverlays, 1);
    s.setInt(kOfxImageEffectPropSupportsMultiResolution, 1);
    s.setInt(kOfxImageEffectPropSupportsTiles, 0);
    s.setInt(kOfxImageEffectPropTemporalClipAccess, 1);
    s.setString(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
    s.resetDim(kOfxImageEffectPropSupportedComponents, 1);
    s.setString(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0);
    s.setString(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral, 1);
    s.setString(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGenerator, 2);
    s.setString(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextTransition, 3);
    s.setString(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 0);
    s.setString(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 1);
    s.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
    s.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
    s.setInt(kOfxImageEffectPropSetableFrameRate, 0);
    s.setInt(kOfxImageEffectPropSetableFielding, 0);
    s.setInt(kOfxImageEffectInstancePropSequentialRender, 0);
    s.setInt(kOfxParamHostPropSupportsCustomInteract, 1);
    s.setInt(kOfxParamHostPropSupportsStringAnimation, 0);
    s.setInt(kOfxParamHostPropSupportsChoiceAnimation, 0);
    s.setInt(kOfxParamHostPropSupportsBooleanAnimation, 0);
    s.setInt(kOfxParamHostPropSupportsCustomAnimation, 0);
    s.setInt(kOfxParamHostPropSupportsParametricAnimation, 0);
    s.setInt(kOfxParamHostPropMaxParameters, -1);
    s.setInt(kOfxParamHostPropMaxPages, 0);
    s.setInt(kOfxParamHostPropPageRowColumnCount, 0, 0);
    s.setInt(kOfxParamHostPropPageRowColumnCount, 0, 1);
    s.setPointer(kOfxPropHostOSHandle, config.mainWindow);
    s.setString(kOfxImageEffectPropOpenGLRenderSupported, (config.gpu && gl::available()) ? "true" : "false");
    s.setInt(kOfxImageEffectPropRenderQualityDraft, 1);
    s.setString(kOfxImageEffectHostPropNativeOrigin, kOfxHostNativeOriginBottomLeft);
    if (config.vegasExtensions) {
        s.setString(kVegasPropHostAppDataDirectory, userDataDir());
        s.setPointer(kVegasPropHostHWnd, config.mainWindow);
        s.setString(kVegasPropSpikeKey, "");
    } else {
        s.erase(kVegasPropHostAppDataDirectory);
        s.erase(kVegasPropHostHWnd);
        s.erase(kVegasPropSpikeKey);
    }
}

std::vector<std::string> Host::vegasProgramDirs()
{
    std::vector<std::string> out;
#ifdef _WIN32
    std::vector<std::string> roots;
    for (const wchar_t* var : {L"ProgramW6432", L"ProgramFiles"}) {
        wchar_t buf[1024];
        DWORD n = GetEnvironmentVariableW(var, buf, 1024);
        if (n > 0 && n < 1024) {
            std::string r = narrow(buf);
            if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
        }
    }
    for (const auto& root : roots)
        for (const char* vendor : {"VEGAS", "Sony"}) {
            std::string v = pathJoin(root, vendor);
            for (const auto& name : listDir(v)) {
                std::string d = pathJoin(v, name);
                if (dirExists(pathJoin(d, "OFX Video Plug-Ins"))) out.push_back(d);
            }
        }
#endif
    return out;
}

std::vector<std::string> Host::defaultSearchDirs()
{
    std::vector<std::string> dirs;
    auto add = [&](const std::string& d) {
        if (d.empty() || !dirExists(d)) return;
        for (const auto& e : dirs) if (toLower(e) == toLower(d)) return;
        dirs.push_back(d);
    };
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"OFX_PLUGIN_PATH", buf, 32768);
    if (n > 0 && n < 32768) {
        std::string all = narrow(buf);
        size_t a = 0;
        while (a <= all.size()) {
            size_t b = all.find(';', a);
            if (b == std::string::npos) b = all.size();
            add(all.substr(a, b - a));
            a = b + 1;
        }
    }
    for (const wchar_t* var : {L"CommonProgramW6432", L"CommonProgramFiles"}) {
        n = GetEnvironmentVariableW(var, buf, 32768);
        if (n > 0 && n < 32768) add(pathJoin(pathJoin(narrow(buf), "OFX"), "Plugins"));
    }
    for (const auto& v : vegasProgramDirs()) add(pathJoin(v, "OFX Video Plug-Ins"));
#else
    const char* env = std::getenv("OFX_PLUGIN_PATH");
    if (env) {
        std::string all = env;
        size_t a = 0;
        while (a <= all.size()) {
            size_t b = all.find(':', a);
            if (b == std::string::npos) b = all.size();
            add(all.substr(a, b - a));
            a = b + 1;
        }
    }
#ifdef __APPLE__
    add("/Library/OFX/Plugins");
#endif
    add("/usr/OFX/Plugins");
    add("/usr/local/OFX/Plugins");
    const char* home = std::getenv("HOME");
    if (home) add(pathJoin(home, ".local/OFX/Plugins"));
#endif
    return dirs;
}

static const char* archDir()
{
#ifdef _WIN32
    return "Win64";
#elif defined(__APPLE__)
    return "MacOS";
#else
    return "Linux-x86-64";
#endif
}

static void findIn(const std::string& dir, int depth, std::vector<std::string>& out)
{
    if (depth > 6) return;
    for (const auto& name : listDir(dir)) {
        std::string p = pathJoin(dir, name);
        if (endsWithNoCase(name, ".ofx.bundle")) {
            std::string bin = pathJoin(pathJoin(p, "Contents"), archDir());
            for (const auto& f : listDir(bin))
                if (endsWithNoCase(f, ".ofx")) out.push_back(pathJoin(bin, f));
        } else if (endsWithNoCase(name, ".ofx") && fileExists(p)) {
            out.push_back(p);
        } else if (dirExists(p)) {
            findIn(p, depth + 1, out);
        }
    }
}

std::vector<std::string> Host::findBinaries(const std::vector<std::string>& dirs)
{
    std::vector<std::string> out;
    for (const auto& d : dirs) findIn(d, 0, out);
    std::vector<std::string> unique;
    std::set<std::string> seen;
    for (const auto& p : out)
        if (seen.insert(toLower(p)).second) unique.push_back(p);
    return unique;
}

thread_local Plugin* Host::currentPlugin = nullptr;

static void dropImages(Instance* inst)
{
    std::lock_guard<std::mutex> lk(inst->imageMutex);
    inst->liveImages.clear();
}

Binary* Host::loadBinary(const std::string& path, std::string* err)
{
    std::lock_guard<std::mutex> loading(loadBinaryMutex);
    {
        std::lock_guard<std::mutex> lk(mutex);
        for (const auto& b : binaries_) if (toLower(b->path) == toLower(path)) return b.get();
    }
    if (isBuiltinPath(path)) {
        std::unique_ptr<Binary> bin(new Binary());
        bin->path = path;
        bin->bundlePath = path;
        bin->builtin = true;
        if (builtinSetHost(&ofxHost) != kOfxStatOK) {
            if (err) *err = "could not be set up";
            return nullptr;
        }
        for (int k = 0, n = builtinNumberOfPlugins(); k < n; ++k) {
            OfxPlugin* op = builtinPlugin(k);
            if (!op) continue;
            std::unique_ptr<Plugin> pl(new Plugin());
            pl->binary = bin.get();
            pl->ofx = op;
            pl->info.id = op->pluginIdentifier;
            pl->info.versionMajor = (int)op->pluginVersionMajor;
            pl->info.versionMinor = (int)op->pluginVersionMinor;
            pl->info.label = op->pluginIdentifier;
            pl->info.binaryPath = bin->path;
            pl->info.bundlePath = bin->bundlePath;
            bin->plugins.push_back(std::move(pl));
        }
        std::lock_guard<std::mutex> lk(mutex);
        binaries_.push_back(std::move(bin));
        return binaries_.back().get();
    }
    static std::once_flag once;
    std::call_once(once, [] { for (const auto& d : vegasProgramDirs()) DynLib::addSearchDir(d); });

    std::unique_ptr<Binary> bin(new Binary());
    bin->path = path;
    std::string up2 = pathDir(pathDir(path)), up3 = pathDir(up2);
    bin->bundlePath = (toLower(pathFileName(up2)) == "contents" && endsWithNoCase(up3, ".ofx.bundle")) ? up3 : pathDir(path);
    std::string e;
    if (!bin->lib.open(path, &e)) {
        if (err) *err = "could not be loaded (" + e + ")";
        return nullptr;
    }
    typedef int (*GetNumFn)(void);
    typedef OfxPlugin* (*GetPluginFn)(int);
    typedef OfxStatus (*SetHostFn)(const OfxHost*);
    GetNumFn getNum = (GetNumFn)bin->lib.symbol("OfxGetNumberOfPlugins");
    GetPluginFn getPlugin = (GetPluginFn)bin->lib.symbol("OfxGetPlugin");
    if (!getNum || !getPlugin) {
        if (err) *err = "is not an OpenFX plug-in (OfxGetNumberOfPlugins / OfxGetPlugin not found)";
        return nullptr;
    }
    SetHostFn setHost = (SetHostFn)bin->lib.symbol("OfxSetHost");
    if (setHost) {
        OfxStatus hs = setHost(&ofxHost);
        if (hs != kOfxStatOK) {
            if (err) *err = std::string("does not accept this host (OfxSetHost: ") + statusName(hs) + ")";
            bin->lib.leak();
            return nullptr;
        }
    }
    int n = getNum();
    if (n < 0 || n > 10000) n = 0;
    for (int k = 0; k < n; ++k) {
        OfxPlugin* op = getPlugin(k);
        if (!op || !op->pluginApi || !op->pluginIdentifier || !op->mainEntry) continue;
        if (std::strcmp(op->pluginApi, kOfxImageEffectPluginApi) != 0) continue;
        std::unique_ptr<Plugin> pl(new Plugin());
        pl->binary = bin.get();
        pl->ofx = op;
        pl->info.id = op->pluginIdentifier;
        pl->info.versionMajor = (int)op->pluginVersionMajor;
        pl->info.versionMinor = (int)op->pluginVersionMinor;
        pl->info.label = op->pluginIdentifier;
        pl->info.binaryPath = bin->path;
        pl->info.bundlePath = bin->bundlePath;
        bin->plugins.push_back(std::move(pl));
    }
    bin->lib.leak();        // plug-ins are never unloaded: many do not survive it
    std::lock_guard<std::mutex> lk(mutex);
    binaries_.push_back(std::move(bin));
    return binaries_.back().get();
}

std::vector<Plugin*> Host::allPlugins() const
{
    std::lock_guard<std::mutex> lk(mutex);
    std::vector<Plugin*> out;
    for (const auto& b : binaries_)
        for (const auto& p : b->plugins) out.push_back(p.get());
    return out;
}

Plugin* Host::findPlugin(const std::string& id) const
{
    std::lock_guard<std::mutex> lk(mutex);
    // A plug-in that failed to load loses to one that did not; a real plug-in (VEGAS's) wins over Reno's built-in of the same identifier (unless
    // Preferences > Video prefers the built-in); then the newest version.
    Plugin* best = nullptr;
    const bool toBuiltin = config.preferBuiltinVegasFx && isVegasOwnEffect(id);
    auto rank = [toBuiltin](const Plugin* p) {
        const bool builtin = p->binary && p->binary->builtin;
        return (p->failed || p->crashed || p->unusable ? 0 : 2) + (builtin == toBuiltin ? 1 : 0);
    };
    for (const auto& b : binaries_)
        for (const auto& p : b->plugins)
            if (p->info.id == id) {
                if (!best || rank(p.get()) > rank(best) ||
                    (rank(p.get()) == rank(best) && (p->info.versionMajor > best->info.versionMajor ||
                    (p->info.versionMajor == best->info.versionMajor && p->info.versionMinor > best->info.versionMinor))))
                    best = p.get();
            }
    return best;
}

// ====================================================================== Plugin
#ifdef _WIN32
static OfxStatus guardedEntry(OfxPluginEntryPoint* f, const char* a, const void* h, OfxPropertySetHandle i, OfxPropertySetHandle o, unsigned long* code)
{
    __try {
        return f(a, h, i, o);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return kOfxStatFailed;
    }
}
#endif

OfxStatus Plugin::call(const char* action, const void* handle, PropertySet* in, PropertySet* out)
{
    if (!ofx || !ofx->mainEntry || crashed) return kOfxStatFailed;
    // what the plug-in asks for and does not get during this action (a nested action keeps the outer one's afterwards)
    MissingTrace& trace = missingTrace();
    const MissingTrace outer = trace;
    trace = MissingTrace();
    trace.plugin = this;
    OfxStatus st;
#ifdef _WIN32
    unsigned long code = 0;
    st = guardedEntry(ofx->mainEntry, action, handle, (OfxPropertySetHandle)in, (OfxPropertySetHandle)out, &code);
    if (code) {
        crashed = true;
        error = strfmt("crashed in %s (exception 0x%08lX)", action, code);
        logf("ofx: %s %s", info.id.c_str(), error.c_str());
    }
#else
    st = ofx->mainEntry(action, handle, (OfxPropertySetHandle)in, (OfxPropertySetHandle)out);
#endif
    if (st == kOfxStatErrMissingHostFeature) {
        // the last suite and property it asked for that Reno did not have
        std::string what;
        if (!trace.suite.empty()) what = "the suite " + trace.suite;
        if (!trace.prop.empty()) what += (what.empty() ? "" : "; ") + std::string("the property ") + trace.prop;
        missingFeature = strfmt("in %s; ", action) + (what.empty() ? std::string("it asked for nothing Reno lacks, so it is something else it checks")
                                                                    : "the last thing it asked for that Reno lacks: " + what);
        logf("ofx: %s said a host feature is missing %s", info.id.c_str(), missingFeature.c_str());
    }
    if (Host::get().config.verbose) logf("ofx: %s <- %s = %s", info.id.c_str(), action, statusName(st));
    trace = outer;
    return st;
}

static void defaultDescriptorProps(PropertySet& s, const Plugin& pl)
{
    s.setString(kOfxPropType, kOfxTypeImageEffect);
    s.setString(kOfxPropLabel, pl.info.id);
    s.setString(kOfxPropShortLabel, pl.info.id);
    s.setString(kOfxPropLongLabel, pl.info.id);
    s.setInt(kOfxPropVersion, pl.info.versionMajor, 0);
    s.setInt(kOfxPropVersion, pl.info.versionMinor, 1);
    s.setString(kOfxPropVersionLabel, strfmt("%d.%d", pl.info.versionMajor, pl.info.versionMinor));
    s.setString(kOfxPropPluginDescription, "");
    s.setString(kOfxImageEffectPluginPropGrouping, "");
    s.setString(kOfxPluginPropFilePath, pl.info.bundlePath);
    s.ensure(kOfxImageEffectPropSupportedContexts, PType::String);
    s.ensure(kOfxImageEffectPropSupportedPixelDepths, PType::String);
    s.ensure(kOfxImageEffectPropClipPreferencesSlaveParam, PType::String);
    s.setInt(kOfxImageEffectPropSupportsMultiResolution, 1);
    s.setInt(kOfxImageEffectPropSupportsTiles, 1);
    s.setInt(kOfxImageEffectPropTemporalClipAccess, 0);
    s.setString(kOfxImageEffectPluginRenderThreadSafety, kOfxImageEffectRenderInstanceSafe);
    s.setInt(kOfxImageEffectPluginPropHostFrameThreading, 0);
    s.setInt(kOfxImageEffectPluginPropSingleInstance, 0);
    s.setInt(kOfxImageEffectPluginPropFieldRenderTwiceAlways, 1);
    s.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
    s.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
    s.setPointer(kOfxImageEffectPluginPropOverlayInteractV1, nullptr);
    s.setString(kOfxImageEffectPropOpenGLRenderSupported, "false");
    s.setInt(kOfxImageEffectInstancePropSequentialRender, 0);
    s.setString(kOfxPropIcon, "", 0);
    s.setString(kOfxPropIcon, "", 1);
    if (Host::get().config.vegasExtensions) s.ensure(kVegasPropUpliftGUID, PType::String);    // empty: effects append
}

bool Plugin::load(std::string* err)
{
    std::lock_guard<std::recursive_mutex> once(loadMutex);
    if (loaded) return true;
    if (failed) { if (err) *err = error; return false; }
    Host& host = Host::get();
    Plugin* prev = host.currentPlugin;
    host.currentPlugin = this;
    auto fail = [&](const std::string& m) {
        failed = true;
        if (error.empty() || !crashed) error = m;
        if (err) *err = error;
        host.currentPlugin = prev;
        return false;
    };
    if (ofx->setHost) ofx->setHost(&host.ofxHost);
    OfxStatus st = call(kOfxActionLoad, nullptr, nullptr, nullptr);
    if (crashed) return fail(error);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) return fail(strfmt("refused to load (%s)", explainStatus(st).c_str()));

    desc.reset(new Descriptor());
    desc->plugin = this;
    defaultDescriptorProps(desc->props, *this);
    st = call(kOfxActionDescribe, desc.get(), nullptr, nullptr);
    if (crashed) return fail(error);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) return fail(strfmt("could not describe itself (%s)", explainStatus(st).c_str()));

    const PropertySet& s = desc->props;
    info.label = s.getString(kOfxPropLabel, 0, info.id);
    info.grouping = s.getString(kOfxImageEffectPluginPropGrouping);
    info.description = s.getString(kOfxPropPluginDescription);
    if (!binary || !binary->builtin) {
        if (info.label.empty() || info.label == info.id || info.grouping.empty() || info.description.empty()) {
            ResourceLabels r;
            if (bundleResourceLabels(info.bundlePath, info.binaryPath, info.id, host.config.hostName, r)) {
                if ((info.label.empty() || info.label == info.id) && !r.label.empty()) info.label = r.label;
                if (info.grouping.empty()) info.grouping = r.group;
                if (info.description.empty()) info.description = r.description;
            }
        }
    }
    if (info.label.empty()) info.label = info.id;
    info.contexts.clear();
    for (int k = 0, n = s.dimension(kOfxImageEffectPropSupportedContexts); k < n; ++k) info.contexts.push_back(s.getString(kOfxImageEffectPropSupportedContexts, k));
    info.depths.clear();
    for (int k = 0, n = s.dimension(kOfxImageEffectPropSupportedPixelDepths); k < n; ++k) info.depths.push_back(s.getString(kOfxImageEffectPropSupportedPixelDepths, k));
    info.multiResolution = s.getInt(kOfxImageEffectPropSupportsMultiResolution, 0, 1) != 0;     // the spec's default is 1
    info.hasHwndInteract = s.getPointer(kVegasPluginPropHWndInteractV1) != nullptr;
    info.hasOverlay = s.getPointer(kOfxImageEffectPluginPropOverlayInteractV1) != nullptr;
    const std::string safe = s.getString(kOfxImageEffectPluginRenderThreadSafety);
    safety = safe == kOfxImageEffectRenderUnsafe ? 0 : (safe == kOfxImageEffectRenderFullySafe ? 2 : 1);
    loaded = true;
    host.currentPlugin = prev;
    return true;
}

bool Plugin::supportsContext(const std::string& context) const
{
    return std::find(info.contexts.begin(), info.contexts.end(), context) != info.contexts.end();
}

std::string Plugin::filterContext() const
{
    if (supportsContext(kOfxImageEffectContextFilter)) return kOfxImageEffectContextFilter;
    if (supportsContext(kOfxImageEffectContextGeneral)) return kOfxImageEffectContextGeneral;
    return std::string();
}

Descriptor* Plugin::describeInContext(const std::string& context, std::string* err)
{
    if (!load(err)) return nullptr;
    std::lock_guard<std::recursive_mutex> once(loadMutex);
    auto it = contextDescs.find(context);
    if (it != contextDescs.end()) return it->second.get();
    if (!supportsContext(context)) {
        if (err) *err = "does not work as a " + context;
        return nullptr;
    }
    Host& host = Host::get();
    Plugin* prev = host.currentPlugin;
    host.currentPlugin = this;
    std::unique_ptr<Descriptor> d(new Descriptor());
    d->plugin = this;
    d->props = desc->props;
    d->props.setString(kOfxImageEffectPropContext, context);
    PropertySet in;
    in.setString(kOfxImageEffectPropContext, context);
    OfxStatus st = call(kOfxImageEffectActionDescribeInContext, d.get(), &in, nullptr);
    host.currentPlugin = prev;
    if (crashed) { if (err) *err = error; return nullptr; }
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        if (st == kOfxStatErrMissingHostFeature) unusable = true;
        if (err) *err = strfmt("could not describe itself as a %s (%s)", context.c_str(), explainStatus(st).c_str());
        return nullptr;
    }
    for (int k = 0, n = d->props.dimension(kVegasPropUpliftGUID); k < n; ++k) {
        const std::string g = d->props.getString(kVegasPropUpliftGUID, k);
        if (!g.empty() && std::find(info.upliftGuids.begin(), info.upliftGuids.end(), g) == info.upliftGuids.end()) info.upliftGuids.push_back(g);
    }
    Descriptor* raw = d.get();
    contextDescs[context] = std::move(d);
    return raw;
}

std::unique_ptr<Instance> Plugin::createInstance(const std::string& context, const InstanceSetup& setup, std::string* err)
{
    Descriptor* cd = describeInContext(context, err);
    if (!cd) return nullptr;

    bool hasByte = false, hasFloat = false;
    for (const auto& d : info.depths) {
        if (d == kOfxBitDepthByte) hasByte = true;
        if (d == kOfxBitDepthFloat) hasFloat = true;
    }
    if (!hasByte && !hasFloat) {
        if (err) *err = "works only with 16-bit pictures, which Reno does not provide yet";
        return nullptr;
    }

    std::unique_ptr<Instance> inst(new Instance());
    inst->isInstance = true;
    inst->plugin = this;
    inst->context_ = context;
    inst->setup = setup;
    inst->depth_ = hasByte ? PixelDepth::Byte : PixelDepth::Float;
    inst->props = cd->props;
    PropertySet& s = inst->props;
    s.setString(kOfxPropType, kOfxTypeImageEffectInstance);
    s.setString(kOfxImageEffectPropContext, context);
    s.setPointer(kOfxPropInstanceData, nullptr);
    inst->setProjectSizeProps(setup.width, setup.height);
    s.setDouble(kOfxImageEffectPropProjectOffset, 0, 0);
    s.setDouble(kOfxImageEffectPropProjectOffset, 0, 1);
    s.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, 1.0);
    s.setDouble(kOfxImageEffectInstancePropEffectDuration, setup.durationFrames);
    s.setDouble(kOfxImageEffectPropFrameRate, setup.fps);
    s.setInt(kOfxPropIsInteractive, 0);
    if (Host::get().config.vegasExtensions)
        s.setString(kVegasPropContext, setup.vegasContext.empty() ? kVegasContextUnknown : setup.vegasContext);

    inst->params.cloneFrom(cd->params);
    Instance* raw = inst.get();
    for (auto& p : inst->params.params) {
        Param* pp = p.get();
        pp->props.owner = pp;
        pp->props.onChanged = [raw, pp](const std::string& name) {
            if (raw->onParamChanged) raw->onParamChanged(pp, name);
        };
    }
    for (const auto& c : cd->clips) {
        std::unique_ptr<ClipInst> ci(new ClipInst());
        ci->inst = raw;
        ci->name = c->name;
        ci->props = c->props;
        ci->isOutput = c->name == kOfxImageEffectOutputClipName;
        ci->optional = c->props.getInt(kOfxImageClipPropOptional) != 0;
        inst->clips.push_back(std::move(ci));
    }
    inst->setupClipProps();

    Host& host = Host::get();
    Plugin* prev = host.currentPlugin;
    host.currentPlugin = this;
    OfxStatus st = call(kOfxActionCreateInstance, raw, nullptr, nullptr);
    host.currentPlugin = prev;
    if (crashed) { if (err) *err = error; return nullptr; }
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        if (st == kOfxStatErrMissingHostFeature) unusable = true;
        if (err) *err = strfmt("could not be created (%s)", explainStatus(st).c_str());
        return nullptr;
    }
    inst->created_ = true;
    return inst;
}

// ====================================================================== Instance
Instance::~Instance()
{
    disposeHwndInteract();
    disposeOverlay();
    while (!paramInteracts_.empty()) disposeParamInteract(const_cast<Param*>(paramInteracts_.begin()->first));
    if (created_ && plugin) gpuInstanceGone(*this);
    if (created_ && plugin) {
        std::lock_guard<std::recursive_mutex> lk(mutex);
        plugin->call(kOfxActionDestroyInstance, this, nullptr, nullptr);
    }
}

bool Instance::scalable() const { return plugin && plugin->info.multiResolution; }

void Instance::setProjectSizeProps(int w, int h)
{
    props.setDouble(kOfxImageEffectPropProjectSize, w, 0);
    props.setDouble(kOfxImageEffectPropProjectSize, h, 1);
    props.setDouble(kOfxImageEffectPropProjectExtent, w, 0);
    props.setDouble(kOfxImageEffectPropProjectExtent, h, 1);
}

ClipInst* Instance::findClip(const std::string& name) const
{
    for (const auto& c : clips) if (c->name == name) return c.get();
    return nullptr;
}

std::vector<std::string> Instance::inputClipNames() const
{
    std::vector<std::string> n;
    for (const auto& c : clips) if (!c->isOutput) n.push_back(c->name);
    return n;
}

void Instance::setClipSource(const std::string& clip, std::function<ImagePtr(double)> fn)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    ClipInst* c = findClip(clip);
    if (!c) return;
    const bool was = c->source != nullptr;
    c->source = std::move(fn);
    const bool now = c->source != nullptr;
    c->props.setInt(kOfxImageClipPropConnected, (c->isOutput || c->source) ? 1 : 0);
    if (was == now || c->isOutput) return;
    clipPrefsDirty = true;
    ++revision;
    if (!created_ || changeDepth > 8) return;
    ++changeDepth;
    PropertySet in;
    in.setString(kOfxPropChangeReason, kOfxChangeUserEdited);
    plugin->call(kOfxActionBeginInstanceChanged, this, &in, nullptr);
    PropertySet in2;
    in2.setString(kOfxPropType, kOfxTypeClip);
    in2.setString(kOfxPropName, c->name);
    in2.setString(kOfxPropChangeReason, kOfxChangeUserEdited);
    in2.setDouble(kOfxPropTime, currentTime);
    in2.setDouble(kOfxImageEffectPropRenderScale, 1.0, 0);
    in2.setDouble(kOfxImageEffectPropRenderScale, 1.0, 1);
    plugin->call(kOfxActionInstanceChanged, this, &in2, nullptr);
    plugin->call(kOfxActionEndInstanceChanged, this, &in, nullptr);
    --changeDepth;
    dropImages(this);
}

void Instance::setupClipProps()
{
    const bool vegas = Host::get().config.vegasExtensions;
    for (auto& c : clips) {
        PropertySet& s = c->props;
        s.setString(kOfxImageEffectPropPixelDepth, depthString(depth_));
        s.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
        s.setString(kOfxImageClipPropUnmappedPixelDepth, depthString(preferred_));
        s.setString(kOfxImageClipPropUnmappedComponents, kOfxImageComponentRGBA);
        s.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
        s.setDouble(kOfxImagePropPixelAspectRatio, 1.0);
        s.setDouble(kOfxImageEffectPropFrameRate, setup.fps);
        s.setDouble(kOfxImageEffectPropFrameRange, 0.0, 0);
        s.setDouble(kOfxImageEffectPropFrameRange, setup.durationFrames, 1);
        s.setString(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
        s.setInt(kOfxImageClipPropConnected, (c->isOutput || c->source) ? 1 : 0);
        s.setDouble(kOfxImageEffectPropUnmappedFrameRange, 0.0, 0);
        s.setDouble(kOfxImageEffectPropUnmappedFrameRange, setup.durationFrames, 1);
        s.setDouble(kOfxImageEffectPropUnmappedFrameRate, setup.fps);
        s.setInt(kOfxImageClipPropContinuousSamples, 0);
        if (vegas) s.setString(kVegasPropPixelOrder, pixelOrder(depth_));
    }
}

void Instance::runClipPreferences()
{
    clipPrefsDirty = false;
    {
        bool hasByte = false, hasFloat = false;
        for (const auto& d : plugin->info.depths) {
            if (d == kOfxBitDepthByte) hasByte = true;
            if (d == kOfxBitDepthFloat) hasFloat = true;
        }
        const PixelDepth base = preferred_ == PixelDepth::Float ? (hasFloat ? PixelDepth::Float : PixelDepth::Byte)
                                                                : (hasByte ? PixelDepth::Byte : PixelDepth::Float);
        if (base != depth_) {
            depth_ = base;
            setupClipProps();
        }
    }
    PropertySet out;
    for (const auto& c : clips) {
        out.setString(std::string("OfxImageClipPropComponents_") + c->name, kOfxImageComponentRGBA);
        out.setString(std::string("OfxImageClipPropDepth_") + c->name, depthString(depth_));
        out.setDouble(std::string("OfxImageClipPropPAR_") + c->name, 1.0);
    }
    out.setDouble(kOfxImageEffectPropFrameRate, setup.fps);
    out.setDouble(kOfxImagePropPixelAspectRatio, 1.0);
    out.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    out.setInt(kOfxImageClipPropContinuousSamples, 0);
    out.setInt(kOfxImageEffectFrameVarying, 0);
    out.setString(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
    OfxStatus st = plugin->call(kOfxImageEffectActionGetClipPreferences, this, nullptr, &out);
    if (st != kOfxStatOK) return;
    std::string d = out.getString(std::string("OfxImageClipPropDepth_") + kOfxImageEffectOutputClipName);
    PixelDepth want = depth_;
    if (d == kOfxBitDepthByte) want = PixelDepth::Byte;
    else if (d == kOfxBitDepthFloat) want = PixelDepth::Float;
    if (want != depth_) {
        depth_ = want;
        setupClipProps();
    }
}

void Instance::changed(Param* p, const char* reason, double time)
{
    if (!created_ || changeDepth > 8) return;
    ++changeDepth;
    PropertySet in;
    in.setString(kOfxPropChangeReason, reason);
    plugin->call(kOfxActionBeginInstanceChanged, this, &in, nullptr);
    PropertySet in2;
    in2.setString(kOfxPropType, kOfxTypeParameter);
    in2.setString(kOfxPropName, p->name);
    in2.setString(kOfxPropChangeReason, reason);
    in2.setDouble(kOfxPropTime, time);
    in2.setDouble(kOfxImageEffectPropRenderScale, 1.0, 0);
    in2.setDouble(kOfxImageEffectPropRenderScale, 1.0, 1);
    plugin->call(kOfxActionInstanceChanged, this, &in2, nullptr);
    plugin->call(kOfxActionEndInstanceChanged, this, &in, nullptr);
    --changeDepth;
    dropImages(this);
    const PropertySet& s = props;
    for (int k = 0, n = s.dimension(kOfxImageEffectPropClipPreferencesSlaveParam); k < n; ++k)
        if (s.getString(kOfxImageEffectPropClipPreferencesSlaveParam, k) == p->name) clipPrefsDirty = true;
}

void pluginSetParam(Param* p)
{
    Instance* inst = (p->set && p->set->owner) ? asInstance(p->set->owner) : nullptr;
    if (!inst) return;
    ++inst->revision;
    if (inst->onParamChanged) inst->onParamChanged(p, "value");
}

void Instance::userSetValue(Param* p, const std::vector<double>& v, double time, bool setKey)
{
    if (!p || p->dim == 0) return;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    std::vector<double> vv = v;
    vv.resize((size_t)p->dim, 0.0);
    if (p->isWhole()) for (double& x : vv) x = std::floor(x + 0.5);
    currentTime = time;
    if (setKey || !p->keys.empty()) {
        const bool fresh = p->keys.find(time) == p->keys.end();
        p->keys[time] = vv;
        if (fresh && Param::newKeyCurve().load() != Param::KeyLinear) p->keyCurves[time] = Param::newKeyCurve().load();
    } else {
        p->value = vv;
    }
    p->props.setInt(kOfxParamPropIsAnimating, p->keys.empty() ? 0 : 1);
    ++revision;
    changed(p, kOfxChangeUserEdited, time);
    if (onParamChanged) onParamChanged(p, "value");
}

void Instance::userSetString(Param* p, const std::string& str, double time, bool setKey)
{
    if (!p || (p->kind != ParamKind::String && p->kind != ParamKind::Custom)) return;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    currentTime = time;
    if (setKey || !p->strKeys.empty()) {
        p->strKeys[time] = str;
        p->props.setInt(kOfxParamPropIsAnimating, 1);
    } else {
        p->str = str;
    }
    ++revision;
    changed(p, kOfxChangeUserEdited, time);
    if (onParamChanged) onParamChanged(p, "value");
}

void Instance::userPushButton(Param* p, double time)
{
    if (!p || p->kind != ParamKind::PushButton) return;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    currentTime = time;
    ++revision;
    changed(p, kOfxChangeUserEdited, time);
}

void Instance::userDeleteKeys(Param* p, double time)
{
    if (!p || (p->keys.empty() && p->strKeys.empty())) return;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (!p->strKeys.empty()) {
        p->str = p->strAt(time);
        p->strKeys.clear();
    }
    if (p->dim > 0) p->value = p->valueAt(time);
    p->keys.clear();
    p->keyCurves.clear();
    p->props.setInt(kOfxParamPropIsAnimating, 0);
    ++revision;
    changed(p, kOfxChangeUserEdited, time);
    if (onParamChanged) onParamChanged(p, "value");
}

bool Instance::userMoveKey(Param* p, double time, double to)
{
    if (!p || p->dim == 0) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto it = p->keys.find(time);
    if (it == p->keys.end()) return false;
    if (to == time) return true;
    const std::vector<double> v = it->second;
    const int curve = p->keyCurve(time);
    p->keys.erase(it);
    p->keyCurves.erase(time);
    p->keys[to] = v;
    if (curve != Param::KeyLinear) p->keyCurves[to] = curve;
    else p->keyCurves.erase(to);
    currentTime = to;
    ++revision;
    changed(p, kOfxChangeUserEdited, to);
    if (onParamChanged) onParamChanged(p, "value");
    return true;
}

bool Instance::userSetKeyCurve(Param* p, double time, int curve)
{
    if (!p || p->dim == 0 || !p->keys.count(time) || curve < Param::KeyLinear || curve > Param::KeyHold) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (curve == Param::KeyLinear) p->keyCurves.erase(time);
    else p->keyCurves[time] = curve;
    ++revision;
    changed(p, kOfxChangeUserEdited, time);
    if (onParamChanged) onParamChanged(p, "value");
    return true;
}

bool Instance::userDeleteKey(Param* p, double time)
{
    if (!p || p->dim == 0) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto it = p->keys.find(time);
    if (it == p->keys.end()) return false;
    if (p->keys.size() == 1) p->value = it->second;
    p->keys.erase(it);
    p->keyCurves.erase(time);
    p->props.setInt(kOfxParamPropIsAnimating, p->keys.empty() ? 0 : 1);
    ++revision;
    changed(p, kOfxChangeUserEdited, time);
    if (onParamChanged) onParamChanged(p, "value");
    return true;
}

void Instance::beginEdit()
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    props.setInt(kOfxPropIsInteractive, 1);
    plugin->call(kOfxActionBeginInstanceEdit, this, nullptr, nullptr);
}

void Instance::endEdit()
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    plugin->call(kOfxActionEndInstanceEdit, this, nullptr, nullptr);
    props.setInt(kOfxPropIsInteractive, 0);
}

bool Instance::gpuChainable() const
{
    return plugin && plugin->binary && plugin->binary->builtin && builtinGpuChainable(plugin->info.id);
}

ImagePtr Instance::render(double time, const RenderOptions& opt, std::string* error)
{
    RenderGate::Hold global;
    if (plugin->safety == 0) {
        const auto w0 = std::chrono::steady_clock::now();
        global = RenderGate::Hold(plugin->renderGate);
        if (profile::enabled()) profile::PluginScope::noteWait(plugin->info.id, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count());
        // waiting for the plug-in would end in threads waiting for each other: this render is left out; the caller shows the input
        if (!global.ok()) {
            if (error) error->clear();
            return nullptr;
        }
    }
    profile::PluginScope prof(plugin->info.id, plugin->info.label, opt.width > 0 && (scalable() || opt.previewSizeForAll), plugin->safety);     // reno_cli --profile
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto fail = [&](const std::string& m) -> ImagePtr {
        if (error) *error = m;
        if (outputImage) outputImage->gpuHold = gpu::pendingHold(outputImage->serial);      // (a frame left on the card by a render that then failed: let it go)
        dropImages(this);
        outputImage.reset();
        return nullptr;
    };
    if (plugin->crashed) return fail(plugin->error);
    abort_ = false;
    struct AbortScope {
        std::atomic<const std::atomic<bool>*>& slot;
        AbortScope(std::atomic<const std::atomic<bool>*>& s, const std::atomic<bool>* a) : slot(s) { slot = a; }
        ~AbortScope() { slot = nullptr; }
    } abortScope(renderAbort, opt.abort);
    currentTime = time;
    if (opt.depth != preferred_) {
        preferred_ = opt.depth;
        setupClipProps();
        clipPrefsDirty = true;
    }
    if (clipPrefsDirty) runClipPreferences();
    int w = setup.width, h = setup.height;
    bool shrunk = false;
    if (opt.width > 0 && opt.height > 0 && (opt.width != w || opt.height != h)) {
        if (opt.mediaSize) {
            w = opt.width;
            h = opt.height;
            shrunk = true;
        } else if (scalable()) {
            w = opt.width;
            h = opt.height;
        } else if (opt.previewSizeForAll && opt.width < w && opt.height < h) {
            w = opt.width;
            h = opt.height;
            shrunk = true;
        }
    }
    renderScale[0] = (setup.width > 0 && !shrunk) ? (double)w / setup.width : 1.0;
    renderScale[1] = (setup.height > 0 && !shrunk) ? (double)h / setup.height : 1.0;
    struct ScaleReset {
        Instance& in;
        ~ScaleReset()
        {
            in.renderScale[0] = in.renderScale[1] = 1.0;
            if (in.canonW > 0) {
                in.canonW = in.canonH = 0;
                in.canonScale[0] = in.canonScale[1] = 1.0;
                in.setProjectSizeProps(in.setup.width, in.setup.height);
            }
        }
    } scaleReset{*this};
    if (shrunk) {
        canonW = w;
        canonH = h;
        canonScale[0] = setup.width > 0 ? (double)w / setup.width : 1.0;
        canonScale[1] = setup.height > 0 ? (double)h / setup.height : 1.0;
        setProjectSizeProps(w, h);
    }
    const bool vegas = Host::get().config.vegasExtensions;

    auto commonArgs = [&](PropertySet& in) {
        in.setDouble(kOfxPropTime, time);
        in.setString(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
        const int win[4] = {0, 0, w, h};
        for (int k = 0; k < 4; ++k) in.setInt(kOfxImageEffectPropRenderWindow, win[k], k);
        in.setDouble(kOfxImageEffectPropRenderScale, renderScale[0], 0);
        in.setDouble(kOfxImageEffectPropRenderScale, renderScale[1], 1);
        if (vegas) {
            // VEGAS gives these to IsIdentity and Render alike; its effects read them in both
            in.setDouble(kVegasPropTimeCode, opt.timelineTime >= 0 ? opt.timelineTime : time);
            in.setInt(kVegasPropViewsToRender, 1);
            in.setInt(kVegasPropRenderView, 0);
        }
    };

    {
        PropertySet in, out;
        commonArgs(in);
        out.setString(kOfxPropName, "");
        out.setDouble(kOfxPropTime, time);
        OfxStatus st = plugin->call(kOfxImageEffectActionIsIdentity, this, &in, &out);
        if (plugin->crashed) return fail(plugin->error);
        if (st == kOfxStatOK) {
            ClipInst* c = findClip(out.getString(kOfxPropName));
            if (c && c->source) {
                ImagePtr img = c->source(out.getDouble(kOfxPropTime, 0, time));
                if (img) img->sync();
                dropImages(this);
                if (img && img->depth != opt.depth) img = img->converted(opt.depth);
                if (img) return img;
            }
        }
    }

    // Reno's own effects write every pixel (builtins.cpp's store()), so the frame need not be cleared first
    outputImage = Image::make(w, h, depth_, !(plugin->binary && plugin->binary->builtin));
    if (!outputImage) return fail("out of memory");

    std::string failMsg;
    auto runActions = [&](bool gl) -> bool {
        PropertySet seq;
        seq.setDouble(kOfxImageEffectPropFrameRange, time, 0);
        seq.setDouble(kOfxImageEffectPropFrameRange, time, 1);
        seq.setDouble(kOfxImageEffectPropFrameStep, 1.0);
        seq.setInt(kOfxPropIsInteractive, opt.interactive ? 1 : 0);
        seq.setDouble(kOfxImageEffectPropRenderScale, renderScale[0], 0);
        seq.setDouble(kOfxImageEffectPropRenderScale, renderScale[1], 1);
        seq.setInt(kOfxImageEffectPropSequentialRenderStatus, 0);
        seq.setInt(kOfxImageEffectPropInteractiveRenderStatus, opt.interactive ? 1 : 0);
        seq.setInt(kOfxImageEffectPropRenderQualityDraft, opt.draft ? 1 : 0);
        seq.setInt(kOfxImageEffectPropOpenGLEnabled, gl ? 1 : 0);
        seq.setInt(kRenoOfxImageEffectPropOpenCLEnabled, 0);
        if (vegas) {
            seq.setInt(kVegasPropViewsToRender, 1);
            seq.setInt(kVegasPropRenderView, 0);
            seq.setString(kVegasPropRenderQuality, opt.draft ? kVegasRenderQualityDraft : (opt.interactive ? kVegasRenderQualityPreview : kVegasRenderQualityBest));
        }
        OfxStatus st = plugin->call(kOfxImageEffectActionBeginSequenceRender, this, &seq, nullptr);
        if (plugin->crashed) { failMsg = plugin->error; return false; }
        if (st != kOfxStatOK && st != kOfxStatReplyDefault) { failMsg = strfmt("would not start rendering (%s)", plugin->explainStatus(st).c_str()); return false; }

        PropertySet in;
        commonArgs(in);
        in.setInt(kOfxImageEffectPropSequentialRenderStatus, 0);
        in.setInt(kOfxImageEffectPropInteractiveRenderStatus, opt.interactive ? 1 : 0);
        in.setInt(kOfxImageEffectPropRenderQualityDraft, opt.draft ? 1 : 0);
        in.setInt(kOfxImageEffectPropOpenGLEnabled, gl ? 1 : 0);
        in.setInt(kRenoOfxImageEffectPropOpenCLEnabled, 0);
        if (vegas) {
            in.setString(kVegasPropRenderQuality, opt.draft ? kVegasRenderQualityDraft : (opt.interactive ? kVegasRenderQualityPreview : kVegasRenderQualityBest));
        }
        st = plugin->call(kOfxImageEffectActionRender, this, &in, nullptr);
        OfxStatus st2 = plugin->crashed ? kOfxStatFailed : plugin->call(kOfxImageEffectActionEndSequenceRender, this, &seq, nullptr);
        (void)st2;
        if (plugin->crashed) { failMsg = plugin->error; return false; }
        if (st != kOfxStatOK) {
            failMsg = strfmt("failed to render (%s)%s%s", plugin->explainStatus(st).c_str(), persistentMessage.empty() ? "" : ": ", persistentMessage.c_str());
            return false;
        }
        return true;
    };

    // On the graphics card when the effect can and the host is set to (gpurender.h); if that fails, on the processor, and the effect is not tried with OpenGL again.
    bool rendered = false;
    if (gpuWanted(*this)) {
        std::string glWhy;
        {
            GpuRender gpu(*this, w, h);
            if (gpu.ready()) {
                rendered = runActions(true) && gpu.readBack(*outputImage);
                if (!rendered) glWhy = failMsg.empty() ? gpu.error() : failMsg;
            } else {
                glWhy = gpu.error();
            }
        }
        if (!rendered) {
            if (plugin->crashed) return fail(plugin->error);
            plugin->glFailed = true;
            logf("gpu: %s: OpenGL rendering failed (%s); using the processor from now on", plugin->info.id.c_str(), glWhy.c_str());
            dropImages(this);
            outputImage = Image::make(w, h, depth_, true);
            if (!outputImage) return fail("out of memory");
            failMsg.clear();
        }
    }
    if (!rendered) {
        // (a built-in effect may leave its picture on the card for the next one: then the host's frame is stale until Image::sync())
        gpu::DeferOutput deferOut(opt.deferGpuOutput && gpuChainable());
        if (!runActions(false)) return fail(failMsg);
    }
    if (outputImage && gpu::isPending(outputImage->serial)) outputImage->gpuHold = gpu::pendingHold(outputImage->serial);

    ImagePtr result = outputImage;
    outputImage.reset();
    dropImages(this);
    if (result->depth != opt.depth) result = result->converted(opt.depth);
    return result;
}

Json Instance::saveState()
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (created_) plugin->call(kOfxActionSyncPrivateData, this, nullptr, nullptr);
    Json j = Json::object();
    j["plugin"] = plugin->info.id;
    j["versionMajor"] = plugin->info.versionMajor;
    j["versionMinor"] = plugin->info.versionMinor;
    Json ps = Json::object();
    for (const auto& p : params.params) {
        if (!p->hasValue()) continue;
        if (!p->props.getInt(kOfxParamPropPersistant, 0, 1)) continue;
        Json e = Json::object();
        if (p->dim > 0) {
            Json v = Json::array();
            for (double x : p->value) v.push(x);
            e["v"] = v;
            // whole numbers (integers, booleans, choices): what a VEGAS project keeps as 32-bit integers
            if (p->isWhole()) e["i"] = true;
            if (!p->keys.empty()) {
                Json ks = Json::array();
                for (const auto& kv : p->keys) {
                    Json k = Json::array();
                    k.push(kv.first);
                    for (double x : kv.second) k.push(x);
                    ks.push(k);
                }
                e["k"] = ks;
                // keys that are not Linear: [[time, curve], ...] (Param::KeyCurve)
                Json kc = Json::array();
                for (const auto& c : p->keyCurves)
                    if (p->keys.count(c.first) && c.second != Param::KeyLinear) {
                        Json one = Json::array();
                        one.push(c.first);
                        one.push(c.second);
                        kc.push(one);
                    }
                if (kc.size() > 0) e["kc"] = kc;
            }
        } else {
            e["s"] = p->str;
            if (!p->strKeys.empty()) {
                Json ks = Json::array();
                for (const auto& kv : p->strKeys) {
                    Json k = Json::array();
                    k.push(kv.first);
                    k.push(kv.second);
                    ks.push(k);
                }
                e["ks"] = ks;
            }
        }
        ps[p->name] = e;
    }
    j["params"] = ps;
    return j;
}

void Instance::loadState(const Json& state)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    const Json& ps = state.get("params");
    // a preset's settings ({"preset": true}, fxlibrary's presetToState) are kept inside each setting's range, as when a preset is picked in the effects
    // window (fxlibrary's applyPreset): VEGAS's presets give Cookie Cutter's Repeat X and Y as 0 (range 1..100), which would cut everything away.
    // A project's settings are taken as saved.
    const bool clampToRange = state.get("preset").asBool();
    for (const auto& m : ps.members()) {
        Param* p = params.find(m.first);
        if (!p || !p->hasValue()) continue;
        const Json& e = m.second;
        if (p->dim > 0) {
            const Json& v = e.get("v");
            for (int k = 0; k < p->dim && k < (int)v.size(); ++k) p->value[(size_t)k] = v.at((size_t)k).asDouble();
            if (p->kind == ParamKind::Double3D && v.size() == 1)
                for (int k = 1; k < p->dim; ++k) p->value[(size_t)k] = p->value[0];
            if (clampToRange)
                for (int k = 0; k < p->dim; ++k) {
                    double& x = p->value[(size_t)k];
                    if (p->props.dimension(kOfxParamPropMin) > k) x = std::max(x, p->props.getDouble(kOfxParamPropMin, k));
                    if (p->props.dimension(kOfxParamPropMax) > k) x = std::min(x, p->props.getDouble(kOfxParamPropMax, k));
                }
            p->keys.clear();
            for (const auto& k : e.get("k").items()) {
                if (k.size() < 1) continue;
                std::vector<double> vals((size_t)p->dim, 0.0);
                for (int d = 0; d < p->dim && d + 1 < (int)k.size(); ++d) vals[(size_t)d] = k.at((size_t)d + 1).asDouble();
                if (p->kind == ParamKind::Double3D && k.size() == 2)
                    for (int d = 1; d < p->dim; ++d) vals[(size_t)d] = vals[0];
                p->keys[k.at(0).asDouble()] = vals;
            }
            p->keyCurves.clear();
            for (const auto& c : e.get("kc").items()) {
                if (c.size() < 2) continue;
                const double t = c.at(0).asDouble();
                const int curve = c.at(1).asInt();
                if (p->keys.count(t) && curve > Param::KeyLinear && curve <= Param::KeyHold) p->keyCurves[t] = curve;
            }
            p->props.setInt(kOfxParamPropIsAnimating, p->keys.empty() ? 0 : 1);
        } else {
            p->str = e.get("s").asString();
            p->strKeys.clear();
            for (const auto& k : e.get("ks").items())
                if (k.size() >= 2) p->strKeys[k.at(0).asDouble()] = k.at(1).asString();
            p->props.setInt(kOfxParamPropIsAnimating, p->strKeys.empty() ? 0 : 1);
        }
    }
    ++revision;
    clipPrefsDirty = true;
    struct Saved { Param* p; std::vector<double> value; std::map<double, std::vector<double>> keys; std::string str; std::map<double, std::string> strKeys; std::map<double, int> curves; };
    std::vector<Saved> saved;
    for (const auto& p : params.params)
        if (p->hasValue() && ps.has(p->name)) saved.push_back({p.get(), p->value, p->keys, p->str, p->strKeys, p->keyCurves});
    for (const auto& sv : saved) changed(sv.p, kOfxChangeUserEdited, currentTime);
    for (const auto& sv : saved) {
        sv.p->value = sv.value;
        sv.p->keys = sv.keys;
        sv.p->str = sv.str;
        sv.p->strKeys = sv.strKeys;
        sv.p->keyCurves = sv.curves;
        sv.p->props.setInt(kOfxParamPropIsAnimating, (sv.p->keys.empty() && sv.p->strKeys.empty()) ? 0 : 1);
    }
    ++revision;
}

// ---------------------------------------------------------------------- the effect's own window (VEGAS)
bool Instance::hasHwndInteract() const
{
    return Host::get().config.vegasExtensions && props.getPointer(kVegasPluginPropHWndInteractV1) != nullptr;
}

#ifdef _WIN32
static OfxStatus guardedInteract(OfxPluginEntryPoint* f, const char* a, const void* h, OfxPropertySetHandle i, OfxPropertySetHandle o)
{
    __try {
        return f(a, h, i, o);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kOfxStatFailed;
    }
}
#else
static OfxStatus guardedInteract(OfxPluginEntryPoint* f, const char* a, const void* h, OfxPropertySetHandle i, OfxPropertySetHandle o)
{
    return f(a, h, i, o);
}
#endif

bool Instance::createHwndInteract(void* parentWindow, int* minW, int* minH, int* prefW, int* prefH)
{
    if (!hasHwndInteract() || !parentWindow) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    disposeHwndInteract();
    hwnd_.reset(new HwndInteractImpl());
    HwndInteractImpl* h = hwnd_.get();
    h->entry = (OfxPluginEntryPoint*)props.getPointer(kVegasPluginPropHWndInteractV1);
    h->props.owner = &h->props;
    PropertySet& s = h->props;
    s.setString(kOfxPropType, "OfxTypeInteract");
    s.setPointer(kOfxPropEffectInstance, this);
    s.setPointer(kOfxPropInstanceData, nullptr);
    s.setDouble(kOfxInteractPropPixelScale, 1.0, 0);
    s.setDouble(kOfxInteractPropPixelScale, 1.0, 1);
    for (int k = 0; k < 3; ++k) s.setDouble(kOfxInteractPropBackgroundColour, 0.0, k);
    s.setInt(kOfxInteractPropBitDepth, 8);
    s.setInt(kOfxInteractPropHasAlpha, 0);
    s.ensure(kOfxInteractPropSlaveToParam, PType::String);

    guardedInteract(h->entry, kOfxActionDescribe, h, nullptr, nullptr);
    OfxStatus st = guardedInteract(h->entry, kOfxActionCreateInstance, h, nullptr, nullptr);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) { hwnd_.reset(); return false; }
    h->instanceMade = true;
    PropertySet in, out;
    in.setPointer(kVegasHWndPropParent, parentWindow);
    for (int k = 0; k < 2; ++k) {
        out.setInt(kVegasHWndPropMinSize, 0, k);
        out.setInt(kVegasHWndPropPreferredSize, 0, k);
    }
    st = guardedInteract(h->entry, kVegasHWndActionCreateWindow, h, (OfxPropertySetHandle)&in, (OfxPropertySetHandle)&out);
    if (st != kOfxStatOK) { disposeHwndInteract(); return false; }
    h->windowMade = true;
    if (minW) *minW = out.getInt(kVegasHWndPropMinSize, 0);
    if (minH) *minH = out.getInt(kVegasHWndPropMinSize, 1);
    if (prefW) *prefW = out.getInt(kVegasHWndPropPreferredSize, 0);
    if (prefH) *prefH = out.getInt(kVegasHWndPropPreferredSize, 1);
    guardedInteract(h->entry, kVegasHWndActionShowWindow, h, nullptr, nullptr);
    return true;
}

void Instance::moveHwndInteract(int x, int y, int w, int h)
{
    if (!hwnd_ || !hwnd_->windowMade) return;
    PropertySet in;
    const int at[4] = {x, y, w, h};
    for (int k = 0; k < 4; ++k) in.setInt(kVegasHWndPropLocation, at[k], k);
    guardedInteract(hwnd_->entry, kVegasHWndActionMoveWindow, hwnd_.get(), (OfxPropertySetHandle)&in, nullptr);
}

void Instance::disposeHwndInteract()
{
    if (!hwnd_) return;
    if (hwnd_->windowMade) guardedInteract(hwnd_->entry, kVegasHWndActionDisposeWindow, hwnd_.get(), nullptr, nullptr);
    if (hwnd_->instanceMade) guardedInteract(hwnd_->entry, kOfxActionDestroyInstance, hwnd_.get(), nullptr, nullptr);
    hwnd_.reset();
}

bool Instance::invokeAbout()
{
    if (!Host::get().config.vegasExtensions) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    return plugin->call(kVegasActionInvokeAbout, this, nullptr, nullptr) == kOfxStatOK;
}

bool Instance::invokeHelp()
{
    if (!Host::get().config.vegasExtensions) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    return plugin->call(kVegasActionInvokeHelp, this, nullptr, nullptr) == kOfxStatOK;
}

// ---------------------------------------------------------------------- on-screen controls (overlay interact)
bool Instance::hasOverlay() const
{
    return props.getPointer(kOfxImageEffectPluginPropOverlayInteractV1) != nullptr;
}

PropertySet* Instance::overlayProps() { return overlay_ ? &overlay_->props : nullptr; }

bool Instance::createOverlay()
{
    if (overlay_) return true;
    if (!hasOverlay() || plugin->crashed) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    OfxPluginEntryPoint* entry = (OfxPluginEntryPoint*)props.getPointer(kOfxImageEffectPluginPropOverlayInteractV1);
    overlayDesc_.reset(new HwndInteractImpl());
    HwndInteractImpl* desc = overlayDesc_.get();
    desc->entry = entry;
    desc->overlayOf = this;
    desc->props.owner = &desc->props;
    desc->props.setInt(kOfxInteractPropHasAlpha, 1);
    desc->props.setInt(kOfxInteractPropBitDepth, 8);
    OfxStatus st = guardedInteract(entry, kOfxActionDescribe, desc, nullptr, nullptr);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        logf("ofx %s: its overlay could not be described (%s)", plugin->info.id.c_str(), statusName(st));
        overlayDesc_.reset();
        return false;
    }
    overlay_.reset(new HwndInteractImpl());
    HwndInteractImpl* h = overlay_.get();
    h->entry = entry;
    h->overlayOf = this;
    h->props.owner = &h->props;
    PropertySet& s = h->props;
    s.setString(kOfxPropType, "OfxTypeInteract");
    s.setPointer(kOfxPropEffectInstance, this);
    s.setPointer(kOfxPropInstanceData, nullptr);
    s.setDouble(kOfxInteractPropPixelScale, 1.0, 0);
    s.setDouble(kOfxInteractPropPixelScale, 1.0, 1);
    for (int k = 0; k < 3; ++k) s.setDouble(kOfxInteractPropBackgroundColour, 0.0, k);
    s.setDouble(kOfxInteractPropSuggestedColour, 1.0, 0);
    s.setDouble(kOfxInteractPropSuggestedColour, 1.0, 1);
    s.setDouble(kOfxInteractPropSuggestedColour, 0.6, 2);
    s.setInt(kRenoOfxInteractPropViewportSize, 0, 0);
    s.setInt(kRenoOfxInteractPropViewportSize, 0, 1);
    s.setInt(kOfxInteractPropHasAlpha, 1);
    s.setInt(kOfxInteractPropBitDepth, 8);
    s.ensure(kOfxInteractPropSlaveToParam, PType::String);
    st = guardedInteract(entry, kOfxActionCreateInstance, h, nullptr, nullptr);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        logf("ofx %s: its overlay could not be made (%s)", plugin->info.id.c_str(), statusName(st));
        overlay_.reset();
        overlayDesc_.reset();
        return false;
    }
    h->instanceMade = true;
    return true;
}

void Instance::disposeOverlay()
{
    if (!overlay_ && !overlayDesc_) return;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (overlay_ && overlay_->instanceMade) guardedInteract(overlay_->entry, kOfxActionDestroyInstance, overlay_.get(), nullptr, nullptr);
    overlay_.reset();
    overlayDesc_.reset();
}

// Calls one of the overlay's actions; an overlay that fails is let go of (OpenFX: "the host should ignore this
// interact in future").
OfxStatus Instance::overlayCall(const char* action, PropertySet* in)
{
    if (!overlay_ || plugin->crashed) return kOfxStatFailed;
    const OfxStatus st = guardedInteract(overlay_->entry, action, overlay_.get(), (OfxPropertySetHandle)in, nullptr);
    if (st == kOfxStatFailed || st == kOfxStatErrFatal) {
        logf("ofx %s: its overlay failed in %s (%s) and is no longer used", plugin->info.id.c_str(), action, statusName(st));
        overlay_->instanceMade = st != kOfxStatErrFatal;     // after a fatal error it is not even told to go
        disposeOverlay();
    }
    return st;
}

static void overlayCommon(PropertySet& in, Instance* inst, const Instance::OverlayArgs& a, bool view)
{
    in.setPointer(kOfxPropEffectInstance, inst);
    in.setDouble(kOfxPropTime, a.time);
    in.setDouble(kOfxImageEffectPropRenderScale, a.renderScale[0], 0);
    in.setDouble(kOfxImageEffectPropRenderScale, a.renderScale[1], 1);
    if (!view) return;
    in.setDouble(kOfxInteractPropPixelScale, a.pixelScale[0], 0);
    in.setDouble(kOfxInteractPropPixelScale, a.pixelScale[1], 1);
    for (int k = 0; k < 3; ++k) in.setDouble(kOfxInteractPropBackgroundColour, a.background[k], k);
}

// the overlay's own properties follow the view too: some effects read them there rather than from the action's
static void overlayView(PropertySet& s, const Instance::OverlayArgs& a)
{
    s.setDouble(kOfxInteractPropPixelScale, a.pixelScale[0], 0);
    s.setDouble(kOfxInteractPropPixelScale, a.pixelScale[1], 1);
    for (int k = 0; k < 3; ++k) s.setDouble(kOfxInteractPropBackgroundColour, a.background[k], k);
    s.setInt(kRenoOfxInteractPropViewportSize, a.viewport[0], 0);
    s.setInt(kRenoOfxInteractPropViewportSize, a.viewport[1], 1);
}

bool Instance::overlayDraw(const OverlayArgs& a)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (!overlay_) return false;
    overlayView(overlay_->props, a);
    PropertySet in;
    overlayCommon(in, this, a, true);
    in.setPointer(kRenoOfxInteractPropDrawContext, nullptr);
    currentTime = a.time;
    return overlayCall(kOfxInteractActionDraw, &in) == kOfxStatOK;
}

bool Instance::overlayPen(const char* action, const OverlayArgs& a, double x, double y, double pressure, int viewX, int viewY)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (!overlay_ || !action) return false;
    overlayView(overlay_->props, a);
    PropertySet in;
    overlayCommon(in, this, a, true);
    in.setDouble(kOfxInteractPropPenPosition, x, 0);
    in.setDouble(kOfxInteractPropPenPosition, y, 1);
    in.setInt(kOfxInteractPropPenViewportPosition, viewX, 0);
    in.setInt(kOfxInteractPropPenViewportPosition, viewY, 1);
    in.setDouble(kOfxInteractPropPenPressure, pressure);
    currentTime = a.time;
    return overlayCall(action, &in) == kOfxStatOK;
}

bool Instance::overlayKey(const char* action, const OverlayArgs& a, int keySym, const std::string& keyString)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (!overlay_ || !action) return false;
    PropertySet in;
    overlayCommon(in, this, a, false);
    in.setInt(kRenoOfxPropKeySym, keySym);
    in.setString(kRenoOfxPropKeyString, keyString);
    currentTime = a.time;
    return overlayCall(action, &in) == kOfxStatOK;
}

bool Instance::overlayFocus(bool gained, const OverlayArgs& a)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    if (!overlay_) return false;
    overlayView(overlay_->props, a);
    PropertySet in;
    overlayCommon(in, this, a, true);
    return overlayCall(gained ? kOfxInteractActionGainFocus : kOfxInteractActionLoseFocus, &in) == kOfxStatOK;
}

// ---------------------------------------------------------------------- parameters' own controls (custom interacts)
bool Instance::paramHasInteract(const Param* p) const
{
    return p && !plugin->crashed && p->props.getPointer(kOfxParamPropInteractV1) != nullptr;
}

void Instance::paramInteractSize(const Param* p, int preferred[2], int minimum[2], double* aspect) const
{
    const PropertySet& s = p->props;
    for (int k = 0; k < 2; ++k) {
        minimum[k] = std::max(1, (int)std::lround(s.getDouble(kOfxParamPropInteractMinimumSize, k, 10)));
        preferred[k] = std::max(minimum[k], s.getInt(kOfxParamPropInteractPreferedSize, k, 10));
    }
    if (aspect) *aspect = s.getDouble(kOfxParamPropInteractSizeAspect, 0, 1.0);
}

bool Instance::paramInteractMade(const Param* p) const { return paramInteracts_.count(p) != 0; }

bool Instance::createParamInteract(Param* p)
{
    if (!p || paramInteracts_.count(p)) return p != nullptr;
    if (!paramHasInteract(p)) return false;
    std::lock_guard<std::recursive_mutex> lk(mutex);
    OfxPluginEntryPoint* entry = (OfxPluginEntryPoint*)p->props.getPointer(kOfxParamPropInteractV1);
    HwndInteractImpl desc;
    desc.entry = entry;
    desc.props.owner = &desc.props;
    desc.props.setInt(kOfxInteractPropHasAlpha, 1);
    desc.props.setInt(kOfxInteractPropBitDepth, 8);
    OfxStatus st = guardedInteract(entry, kOfxActionDescribe, &desc, nullptr, nullptr);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        logf("ofx %s: the control of %s could not be described (%s)", plugin->info.id.c_str(), p->name.c_str(), statusName(st));
        return false;
    }
    std::unique_ptr<HwndInteractImpl> h(new HwndInteractImpl());
    h->entry = entry;
    h->paramInst = this;
    h->paramOf = p;
    h->props.owner = &h->props;
    PropertySet& s = h->props;
    s.setString(kOfxPropType, "OfxTypeInteract");
    s.setPointer(kOfxPropEffectInstance, this);
    s.setPointer(kOfxPropInstanceData, nullptr);
    s.setDouble(kOfxInteractPropPixelScale, 1.0, 0);
    s.setDouble(kOfxInteractPropPixelScale, 1.0, 1);
    for (int k = 0; k < 3; ++k) s.setDouble(kOfxInteractPropBackgroundColour, 0.0, k);
    s.setInt(kRenoOfxInteractPropViewportSize, 0, 0);
    s.setInt(kRenoOfxInteractPropViewportSize, 0, 1);
    s.setInt(kOfxInteractPropHasAlpha, 1);
    s.setInt(kOfxInteractPropBitDepth, 8);
    s.setString(kOfxInteractPropSlaveToParam, p->name, 0);
    st = guardedInteract(entry, kOfxActionCreateInstance, h.get(), nullptr, nullptr);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        logf("ofx %s: the control of %s could not be made (%s)", plugin->info.id.c_str(), p->name.c_str(), statusName(st));
        return false;
    }
    h->instanceMade = true;
    paramInteracts_[p] = std::move(h);
    return true;
}

void Instance::disposeParamInteract(Param* p)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto it = paramInteracts_.find(p);
    if (it == paramInteracts_.end()) return;
    std::unique_ptr<HwndInteractImpl> h = std::move(it->second);
    paramInteracts_.erase(it);
    if (h->instanceMade && !plugin->crashed) guardedInteract(h->entry, kOfxActionDestroyInstance, h.get(), nullptr, nullptr);
}

OfxStatus Instance::paramInteractCall(Param* p, const char* action, PropertySet* in)
{
    auto it = paramInteracts_.find(p);
    if (it == paramInteracts_.end() || plugin->crashed) return kOfxStatFailed;
    HwndInteractImpl* h = it->second.get();
    const OfxStatus st = guardedInteract(h->entry, action, h, (OfxPropertySetHandle)in, nullptr);
    if (st == kOfxStatFailed || st == kOfxStatErrFatal) {
        logf("ofx %s: the control of %s failed in %s (%s) and is no longer used", plugin->info.id.c_str(), p->name.c_str(), action, statusName(st));
        h->instanceMade = st != kOfxStatErrFatal;
        disposeParamInteract(p);
    }
    return st;
}

static void paramInteractCommon(PropertySet& in, PropertySet& own, Instance* inst, const Instance::ParamInteractArgs& a)
{
    in.setPointer(kOfxPropEffectInstance, inst);
    in.setDouble(kOfxPropTime, a.time);
    in.setDouble(kOfxImageEffectPropRenderScale, 1.0, 0);
    in.setDouble(kOfxImageEffectPropRenderScale, 1.0, 1);
    for (PropertySet* s : {&in, &own}) {
        s->setDouble(kOfxInteractPropPixelScale, a.pixelScale[0], 0);
        s->setDouble(kOfxInteractPropPixelScale, a.pixelScale[1], 1);
        for (int k = 0; k < 3; ++k) s->setDouble(kOfxInteractPropBackgroundColour, a.background[k], k);
        s->setInt(kRenoOfxInteractPropViewportSize, a.size[0], 0);
        s->setInt(kRenoOfxInteractPropViewportSize, a.size[1], 1);
    }
}

bool Instance::paramInteractDraw(Param* p, const ParamInteractArgs& a)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto it = paramInteracts_.find(p);
    if (it == paramInteracts_.end()) return false;
    PropertySet in;
    paramInteractCommon(in, it->second->props, this, a);
    in.setPointer(kRenoOfxInteractPropDrawContext, nullptr);
    p->props.setDouble(kOfxParamPropInteractSize, a.size[0], 0);
    p->props.setDouble(kOfxParamPropInteractSize, a.size[1], 1);
    currentTime = a.time;
    return paramInteractCall(p, kOfxInteractActionDraw, &in) == kOfxStatOK;
}

bool Instance::paramInteractPen(Param* p, const char* action, const ParamInteractArgs& a, double x, double y, double pressure)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto it = paramInteracts_.find(p);
    if (it == paramInteracts_.end() || !action) return false;
    PropertySet in;
    paramInteractCommon(in, it->second->props, this, a);
    in.setDouble(kOfxInteractPropPenPosition, x, 0);
    in.setDouble(kOfxInteractPropPenPosition, y, 1);
    in.setInt(kOfxInteractPropPenViewportPosition, (int)std::lround(x), 0);
    in.setInt(kOfxInteractPropPenViewportPosition, (int)std::lround(y), 1);
    in.setDouble(kOfxInteractPropPenPressure, pressure);
    currentTime = a.time;
    return paramInteractCall(p, action, &in) == kOfxStatOK;
}

bool Instance::paramInteractKey(Param* p, const char* action, const ParamInteractArgs& a, int keySym, const std::string& keyString)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto it = paramInteracts_.find(p);
    if (it == paramInteracts_.end() || !action) return false;
    PropertySet in;
    paramInteractCommon(in, it->second->props, this, a);
    in.setInt(kRenoOfxPropKeySym, keySym);
    in.setString(kRenoOfxPropKeyString, keyString);
    currentTime = a.time;
    return paramInteractCall(p, action, &in) == kOfxStatOK;
}

bool Instance::paramInteractFocus(Param* p, bool gained, const ParamInteractArgs& a)
{
    std::lock_guard<std::recursive_mutex> lk(mutex);
    auto it = paramInteracts_.find(p);
    if (it == paramInteracts_.end()) return false;
    PropertySet in;
    paramInteractCommon(in, it->second->props, this, a);
    return paramInteractCall(p, gained ? kOfxInteractActionGainFocus : kOfxInteractActionLoseFocus, &in) == kOfxStatOK;
}

}  // namespace ofx
}  // namespace reno
