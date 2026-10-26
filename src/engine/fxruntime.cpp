#include "fxruntime.h"
#include "audionews.h"
#include "catalog.h"
#include "fxnames.h"
#include "../ofx/vegas_ext.h"
#include "../ofx/builtins.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

namespace reno {

ofx::Plugin* findVideoPlugin(const std::string& id, std::string* error)
{
    ofx::Host& host = ofx::Host::get();
    host.loadBinary(ofx::kBuiltinBinaryPath, nullptr);     // Reno's built-in plug-ins are always there (once)
    ofx::Plugin* pl = host.findPlugin(id);
    // Preferences > Video, "always use Reno's built-in versions": VEGAS's own effects are not looked for as files at all.
    const bool builtinFirst = host.config.preferBuiltinVegasFx && ofx::isVegasOwnEffect(id);
    if (pl && pl->binary && pl->binary->builtin && !builtinFirst) pl = nullptr;     // a real one of the same identifier is looked for first
    if (!pl) {
        VideoFxEntry e;
        if (Catalog::get().findVideo(id, &e) && Catalog::get().crashedWhenTried(e.path)) {
            if (error) *error = pathFileName(e.path) + " crashed when it was last tested (Options > Check All Plug-Ins Again retries it)";
            return nullptr;
        }
        if (Catalog::get().findVideo(id, &e)) {
            std::string err;
            if (!host.loadBinary(e.path, &err)) {
                if (error) *error = pathFileName(e.path) + " " + err;
                return nullptr;
            }
            pl = host.findPlugin(id);
        }
    }
    if (!pl) {
        // not in the catalog: look through the standard folders (once per file; loadBinary remembers)
        static std::once_flag once;
        std::call_once(once, [&] {
            for (const auto& f : ofx::Host::findBinaries(ofx::Host::defaultSearchDirs()))
                if (!Catalog::get().crashedWhenTried(f)) host.loadBinary(f, nullptr);
        });
        pl = host.findPlugin(id);
    }
    if (!pl) pl = host.findPlugin(id);
    if (!pl) {
        if (error) *error = "the effect " + id + " is not installed";
        return nullptr;
    }
    if (!pl->load(error)) {
        // VEGAS's plug-in does not work here: Reno's built-in stand-in of the same identifier takes its place
        ofx::Plugin* other = host.findPlugin(id);
        if (other && other != pl && other->load(nullptr)) {
            if (error) error->clear();
            return other;
        }
        return nullptr;
    }
    return pl;
}

namespace {
std::mutex gNewsMutex;
std::function<void(AudioPlugin*, int)>* gParamNews = nullptr;
std::function<void(AudioPlugin*, int, int)>* gResizeNews = nullptr;
}  // namespace

void setAudioPluginNews(std::function<void(AudioPlugin*, int)> paramChanged, std::function<void(AudioPlugin*, int, int)> editorResized)
{
    std::lock_guard<std::mutex> lk(gNewsMutex);
    gParamNews = new std::function<void(AudioPlugin*, int)>(std::move(paramChanged));
    gResizeNews = new std::function<void(AudioPlugin*, int, int)>(std::move(editorResized));
}

static void forwardNews(AudioPlugin* pl)
{
    if (!pl) return;
    pl->onParamChangedByPlugin = [pl](int index) {
        std::function<void(AudioPlugin*, int)> f;
        {
            std::lock_guard<std::mutex> lk(gNewsMutex);
            if (gParamNews) f = *gParamNews;
        }
        if (f) f(pl, index);
    };
    pl->onEditorResize = [pl](int w, int h) {
        std::function<void(AudioPlugin*, int, int)> f;
        {
            std::lock_guard<std::mutex> lk(gNewsMutex);
            if (gResizeNews) f = *gResizeNews;
        }
        if (f) f(pl, w, h);
    };
}

FxRuntime::~FxRuntime()
{
    clear();
    std::lock_guard<std::mutex> lk(mutex_);
    retiredVideo_.clear();
    retiredAudio_.clear();
}

void FxRuntime::clear()
{
    std::vector<VideoFx> v;
    std::vector<AudioFx> a;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& kv : video_) v.push_back(std::move(kv.second));
        for (auto& kv : audio_) a.push_back(std::move(kv.second));
        video_.clear();
        audio_.clear();
        errors_.clear();
        fallbacks_.clear();
    }
    retire(std::move(v), std::move(a));
}

// Renders use instances without the lock, so an instance taken out of the maps may still be in use: it is destroyed
// only when no render holds the runtime.
void FxRuntime::retire(std::vector<VideoFx>&& v, std::vector<AudioFx>&& a)
{
    if (!v.empty() || !a.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& x : v) retiredVideo_.push_back(std::move(x));
        for (auto& x : a) retiredAudio_.push_back(std::move(x));
    }
    freeRetired();
}

void FxRuntime::freeRetired()
{
    std::vector<VideoFx> v;
    std::vector<AudioFx> a;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (holds_.load() != 0) return;
        v.swap(retiredVideo_);
        a.swap(retiredAudio_);
    }
    // destroyed outside the lock: a plug-in's destructor can take its time.
}

static thread_local int tLane = 0;
void FxRuntime::setThreadLane(int lane) { tLane = std::max(0, std::min(kLanes - 1, lane)); }
int FxRuntime::threadLane() { return tLane; }

ofx::Instance* FxRuntime::video(uint64_t id) const
{
    ofx::Instance* live = nullptr;
    Lane* lane = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = video_.find(id);
        if (it == video_.end()) return nullptr;
        live = it->second.inst.get();
        if (tLane > 0 && tLane < kLanes) lane = it->second.lanes[tLane].get();
    }
    if (!live || !lane || !lane->inst) return live;
    const uint64_t rev = live->revision.load();
    if (lane->synced.load() != rev) {
        std::unique_lock<std::recursive_mutex> busy(live->mutex, std::try_to_lock);
        if (!busy.owns_lock()) return live;
        lane->inst->loadState(live->saveState());
        lane->synced = rev;
        ++laneSyncs_;
    }
    return lane->inst.get();
}

int FxRuntime::laneInstances() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    int n = 0;
    for (const auto& kv : video_)
        for (int i = 1; i < kLanes; ++i)
            if (kv.second.lanes[i] && kv.second.lanes[i]->inst) ++n;
    return n;
}

// One lane instance of a slot's effect: made like the live one and given its settings.
bool FxRuntime::makeLane(uint64_t id, int lane)
{
    if (lane < 1 || lane >= kLanes) return true;
    ofx::Plugin* pl = nullptr;
    ofx::Instance* live = nullptr;
    std::string context;
    ofx::InstanceSetup setup;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = video_.find(id);
        if (it == video_.end() || !it->second.inst || !it->second.plugin) return true;
        VideoFx& v = it->second;
        if (v.lanes[lane] || v.laneFailed[lane] || v.plugin->safety == 0) return true;
        pl = v.plugin;
        live = v.inst.get();
        context = v.context;
        setup = v.setup;
    }
    const uint64_t rev = live->revision.load();
    const Json state = live->saveState();
    std::string err;
    std::unique_ptr<ofx::Instance> inst = pl->createInstance(context, setup, &err);
    if (inst) inst->loadState(state);
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = video_.find(id);
    if (it == video_.end() || it->second.inst.get() != live) return true;
    if (!inst) {
        it->second.laneFailed[lane] = true;
        logf("effect %s: could not create a second instance for parallel rendering (%s); frames are rendered one at a time", pl->info.id.c_str(), err.c_str());
        return true;
    }
    it->second.lanes[lane].reset(new Lane());
    it->second.lanes[lane]->inst = std::move(inst);
    it->second.lanes[lane]->synced = rev;
    return true;
}

bool FxRuntime::prepareLanes(const Project& p, int lanes, double from, double span, double budgetMs)
{
    lanes = std::min(lanes, kLanes);
    if (lanes <= 1) return true;
    const auto start = std::chrono::steady_clock::now();
    bool done = true, stop = false;
    auto one = [&](const FxSlot& s) {
        if (stop || s.kind != "ofx" || s.bypass) return;
        for (int l = 1; l < lanes; ++l) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = video_.find(s.id);
                if (it == video_.end() || !it->second.inst || !it->second.plugin || it->second.plugin->safety == 0 || it->second.lanes[l] ||
                    it->second.laneFailed[l])
                    continue;
            }
            makeLane(s.id, l);
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs) {
                stop = true;
                done = false;
                return;
            }
        }
    };
    auto chain = [&](const FxChain& c) { for (const auto& s : c) one(s); };
    // the events that show in the window, soonest first
    std::vector<const Event*> events;
    for (const auto& t : p.tracks)
        for (const auto& e : t.events)
            if (e.end() > from - 0.25 && e.start < from + span) events.push_back(&e);
    std::sort(events.begin(), events.end(), [](const Event* a, const Event* b) { return a->start < b->start; });
    chain(p.videoOut);
    for (const auto& t : p.tracks) chain(t.fx);
    for (const Event* e : events) {
        chain(e->fx);
        if (e->transition.kind == "ofx") one(e->transition);
        if (const MediaItem* m = p.findMedia(e->mediaId))
            if (m->generated && m->generator.kind == "ofx") one(m->generator);
    }
    return done;
}

AudioPlugin* FxRuntime::audio(uint64_t id) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = audio_.find(id);
    return it == audio_.end() ? nullptr : it->second.plugin.get();
}

AudioPlugin* FxRuntime::audioExtra(uint64_t id, int pair) const
{
    if (pair < 1 || pair > 2) return nullptr;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = audio_.find(id);
    return it == audio_.end() ? nullptr : it->second.extra[pair - 1].get();
}

ofx::RenderGate& FxRuntime::renderLock(const ofx::Instance* inst)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto& m = renderLocks_[inst];
    if (!m) m.reset(new ofx::RenderGate());
    return *m;
}

std::string FxRuntime::error(uint64_t id) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = errors_.find(id);
    return it == errors_.end() ? std::string() : it->second;
}

std::string FxRuntime::fallback(uint64_t slotId) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = fallbacks_.find(slotId);
    return it == fallbacks_.end() ? std::string() : it->second;
}

void FxRuntime::setError(uint64_t id, const std::string& e)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (e.empty()) errors_.erase(id);
    else errors_[id] = e;
}

uint64_t FxRuntime::videoRevision() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    uint64_t r = 0;
    for (const auto& kv : video_)
        if (kv.second.inst) r += kv.second.inst->revision.load() * 1000003ull + kv.first;
    return r;
}

void FxRuntime::prepareVideo(const FxSlot& s, const Project& p, const char* role, double durationFrames)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = video_.find(s.id);
        if (it != video_.end()) {
            ofx::Instance* have = it->second.inst.get();
            // up to date unless made for another frame size or rate, or for the other choice of VEGAS plug-in / built-in
            const bool sourceSame = it->second.builtinChosen == ofx::Host::get().config.preferBuiltinVegasFx || !ofx::isVegasOwnEffect(s.pluginId);
            if (!have || (sourceSame && have->setup.width == p.settings.width && have->setup.height == p.settings.height &&
                          std::fabs(have->setup.fps - p.settings.fps()) < 1e-9)) return;
        }
    }
    making(s);
    Json carried;
    {
        std::vector<VideoFx> old;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = video_.find(s.id);
            if (it != video_.end()) { old.push_back(std::move(it->second)); video_.erase(it); }
        }
        if (!old.empty() && old[0].inst) carried = old[0].inst->saveState();
        retire(std::move(old), std::vector<AudioFx>());
    }
    VideoFx fx;
    fx.builtinChosen = ofx::Host::get().config.preferBuiltinVegasFx;
    std::string err, fellBack;
    ofx::Plugin* pl = findVideoPlugin(s.pluginId, &err);
    if (pl) {
        std::string context;
        const char* vegas = kVegasContextEvent;
        if (!std::strcmp(role, "generator")) { context = kOfxImageEffectContextGenerator; vegas = kVegasContextGenerator; }
        else if (!std::strcmp(role, "transition")) { context = kOfxImageEffectContextTransition; vegas = kVegasContextEventFadeIn; }
        else {
            context = pl->filterContext();
            vegas = !std::strcmp(role, "track") ? kVegasContextTrack : (!std::strcmp(role, "project") ? kVegasContextProject : kVegasContextEvent);
        }
        if (context.empty() || !pl->supportsContext(context)) err = "cannot be used this way";
        else {
            ofx::InstanceSetup setup;
            setup.width = p.settings.width;
            setup.height = p.settings.height;
            setup.fps = p.settings.fps();
            setup.durationFrames = durationFrames;
            setup.vegasContext = vegas;
            fx.plugin = pl;
            fx.context = context;
            fx.setup = setup;
            fx.inst = pl->createInstance(context, setup, &err);
            if (!fx.inst && !(pl->binary && pl->binary->builtin)) {
                // The plug-in's effect cannot be made here (VEGAS's own, for one, may say what it misses of its
                // host).
                ofx::Plugin* other = nullptr;
                if (ofx::Binary* bb = ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath, nullptr))
                    for (const auto& up : bb->plugins) if (up && up->info.id == s.pluginId) other = up.get();
                std::string err2;
                if (other && other != pl && other->binary && other->binary->builtin && other->load(nullptr) && other->supportsContext(context))
                    fx.inst = other->createInstance(context, setup, &err2);
                if (fx.inst) {
                    fx.plugin = other;
                    logf("effect %s: %s; Reno's own is used instead", s.pluginId.c_str(), err.c_str());
                    fellBack = pl->binary ? pathFileName(pl->binary->path) + " " + err : err;
                }
            }
            if (fx.inst && carried.isObject()) fx.inst->loadState(carried);
            else if (fx.inst && s.state.isObject()) fx.inst->loadState(s.state);
            if (fx.inst) {
                const uint64_t slotId = s.id;
                fx.inst->onParamChanged = [this, slotId](ofx::Param* prm, const std::string& what) {
                    if (onVideoParamChanged) onVideoParamChanged(slotId, prm, what);
                };
            }
        }
    }
    if (!fx.inst) {
        fx.error = err.empty() ? "could not be made" : err;
        logf("effect %s: %s", fxSlotLabel(s).c_str(), fx.error.c_str());
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (!fx.inst) errors_[s.id] = fx.error;
    else errors_.erase(s.id);
    if (fellBack.empty()) fallbacks_.erase(s.id);
    else fallbacks_[s.id] = fellBack;
    video_[s.id] = std::move(fx);
}

void FxRuntime::prepareAudio(const FxSlot& s, const Project& p)
{
    AudioFx* have = nullptr;
    // VEGAS's own eFX effects (VST2 ids MX21..MX2B): Reno's reproduction or the installed plug-in, as Preferences says
    const bool efxSlot = s.audio.format == "VST2" && efxReproducedId(s.audio.uid);
    const bool efxBuiltin = efxSlot && efxPreferBuiltin();
    std::string carried;                                    // the settings of an instance made again because that choice changed
    std::vector<AudioFx> old;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = audio_.find(s.id);
        if (it != audio_.end()) have = &it->second;
        if (have && efxSlot && have->efxBuiltin != efxBuiltin) {
            carried = have->plugin ? have->plugin->saveState() : have->keptState;
            old.push_back(std::move(it->second));
            audio_.erase(it);
            errors_.erase(s.id);
            have = nullptr;
        }
    }
    if (!old.empty()) retire(std::vector<VideoFx>(), std::move(old));
    if (have) {
        if (have->plugin && have->sampleRate != p.settings.sampleRate) {
            have->plugin->activate(p.settings.sampleRate, kAudioBlock);
            for (auto& x : have->extra)
                if (x) x->activate(p.settings.sampleRate, kAudioBlock);
            have->sampleRate = p.settings.sampleRate;
        }
        return;
    }
    making(s);
    AudioFx fx;
    std::string err;
    AudioPluginDesc desc = s.audio;
    if (!efxBuiltin && desc.path.empty() && desc.format != "DX") {
        // a project from elsewhere, e.g. a VEGAS project, may name the plug-in only by its id: find its file among the installed ones.
        for (const auto& e : Catalog::get().audio())
            if (e.desc.format == desc.format && e.desc.uid == desc.uid) { desc.path = e.desc.path; break; }
        if (desc.path.empty())
            for (const auto& e : Catalog::get().audio())
                if (e.desc.format == desc.format && toLower(e.desc.name) == toLower(desc.name)) { desc = e.desc; break; }
    }
    if (efxBuiltin) fx.plugin = efxCreate(desc, &err);
    else if (!desc.path.empty() && Catalog::get().crashedWhenTried(desc.path))
        err = pathFileName(desc.path) + " crashed when it was last tested (Options > Check All Plug-Ins Again retries it)";
    else if (desc.path.empty() && (desc.format == "VST2" || desc.format == "VST3")) {
        // not installed (or not where Reno looks): say which plug-in and, for VEGAS's own, which file it is.
        const std::string hint = audioPluginFileHint(desc);
        err = "is not installed: Reno found no " + desc.format + " plug-in " +
              (desc.format == "VST2" ? "with the id '" + vstIdText(desc.uid) + "'" : "with the class id " + desc.uid) +
              (desc.name.empty() ? std::string() : " (\"" + desc.name + "\")") + " in the plug-in folders it looks in" +
              (hint.empty() ? std::string(".") : ". The file it needs is " + hint + ".") +
              " Options > Look for New Plug-Ins looks again.";
    } else fx.plugin = createAudioPlugin(desc, &err);
    if (!fx.plugin && desc.format == "DX" && !err.empty()) {
        const std::string hint = audioPluginFileHint(desc);
        if (!hint.empty()) err += ". Its file is " + hint;
    }
    fx.desc = desc;
    forwardNews(fx.plugin.get());
    if (fx.plugin && (fx.plugin->desc.bridged32 || efxIsReproduction(fx.plugin->desc)))
        logf("audio effect %s: %s%s", s.name.c_str(), efxIsReproduction(fx.plugin->desc) ? "Reno's version" : pathFileName(desc.path).c_str(),
             audioPluginKindNote(fx.plugin->desc).c_str());
    fx.efxBuiltin = fx.plugin && efxIsReproduction(fx.plugin->desc);
    if (fx.plugin) {
        if (!carried.empty() && fx.plugin->loadState(carried)) {
        } else if (s.state.isString() && !s.state.asString().empty() && !fx.plugin->loadState(s.state.asString()))
            logf("audio effect %s: could not load its saved settings; using defaults", s.name.c_str());
        if (!fx.plugin->activate(p.settings.sampleRate, kAudioBlock)) {
            err = "would not start processing";
            fx.plugin.reset();
        }
        fx.sampleRate = p.settings.sampleRate;
    }
    if (!fx.plugin) {
        fx.error = err.empty() ? "could not be made" : err;
        fx.keptState = carried;
        logf("audio effect %s: %s", s.name.c_str(), fx.error.c_str());
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (!fx.plugin) errors_[s.id] = fx.error;
    else errors_.erase(s.id);
    audio_[s.id] = std::move(fx);
}

void FxRuntime::prepareAudioExtras(const FxSlot& s, const Project& p)
{
    AudioPlugin* main = nullptr;
    AudioPluginDesc desc;
    bool need[2] = {false, false};
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = audio_.find(s.id);
        if (it == audio_.end() || !it->second.plugin) return;
        main = it->second.plugin.get();
        desc = it->second.desc;
        for (int k = 0; k < 2; ++k) need[k] = !it->second.extra[k];
    }
    for (int k = 0; k < 2; ++k) {
        if (!need[k]) continue;
        std::string err;
        std::unique_ptr<AudioPlugin> pl = createAudioPlugin(desc, &err);
        if (!pl) {
            logf("audio effect %s: no instance for the surround channels: %s", s.name.c_str(), err.c_str());
            return;
        }
        forwardNews(pl.get());
        const std::string state = main->saveState();
        if (!state.empty()) pl->loadState(state);
        else if (s.state.isString() && !s.state.asString().empty()) pl->loadState(s.state.asString());
        if (!pl->activate(p.settings.sampleRate, kAudioBlock)) return;
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = audio_.find(s.id);
        if (it == audio_.end() || it->second.plugin.get() != main) return;
        it->second.extra[k] = std::move(pl);
    }
}

void FxRuntime::prepare(const Project& p)
{
    if (preparing_ > 0) {
        again_ = std::make_shared<Project>(p);
        return;
    }
    ++preparing_;
    if (!onMaking) prepareNow(p);
    else {
        const Project copy = p;
        madeSoFar_ = 0;
        toMake_ = countNew(copy);
        prepareNow(copy);
    }
    while (again_) {
        std::shared_ptr<Project> next = std::move(again_);
        again_.reset();
        madeSoFar_ = 0;
        toMake_ = onMaking ? countNew(*next) : 0;
        prepareNow(*next);
    }
    --preparing_;
}

int FxRuntime::countNew(const Project& p) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    int n = 0;
    auto one = [&](const FxSlot& s) {
        if (s.kind == "ofx" && !video_.count(s.id)) ++n;
        else if (s.kind == "audio" && !audio_.count(s.id)) ++n;
    };
    auto chain = [&](const FxChain& c) { for (const auto& s : c) one(s); };
    for (const auto& m : p.media)
        if (m.generated && m.generator.kind == "ofx") one(m.generator);
    for (const auto& t : p.tracks) {
        chain(t.fx);
        for (const auto& e : t.events) {
            chain(e.fx);
            if (e.transition.kind == "ofx") one(e.transition);
        }
    }
    chain(p.videoOut);
    chain(p.audioOut);
    for (const auto& b : p.buses) chain(b.fx);
    return n;
}

void FxRuntime::making(const FxSlot& s)
{
    if (!onMaking) return;
    toMake_ = std::max(toMake_, madeSoFar_ + 1);
    const std::function<void(const FxSlot&, int, int)> tell = onMaking;
    const FxSlot slot = s;
    tell(slot, madeSoFar_, toMake_);
    ++madeSoFar_;
}

void FxRuntime::prepareNow(const Project& p)
{
    freeRetired();
    const double fps = p.settings.fps();
    const double projectFrames = std::max(1.0, p.length() * fps);
    auto chain = [&](const FxChain& c, const char* role, double frames) {
        for (const auto& s : c) {
            if (s.kind == "ofx") prepareVideo(s, p, role, frames);
            else if (s.kind == "audio") prepareAudio(s, p);
        }
    };
    for (const auto& m : p.media)
        if (m.generated && m.generator.kind == "ofx") prepareVideo(m.generator, p, "generator", 1.0e6);
    for (const auto& t : p.tracks) {
        chain(t.fx, "track", projectFrames);
        for (const auto& e : t.events) {
            chain(e.fx, "event", std::max(1.0, e.length * fps));
            if (e.transition.kind == "ofx") prepareVideo(e.transition, p, "transition", std::max(1.0, e.length * fps));
        }
    }
    chain(p.videoOut, "project", projectFrames);
    chain(p.audioOut, "project", projectFrames);
    prepareSurround(p);
    for (const auto& b : p.buses) chain(b.fx, "project", projectFrames);
}

void FxRuntime::prepareSurround(const Project& p)
{
    // A 5.1 project's master effects: each is asked to process the six channels as one.
    for (const auto& s : p.audioOut) {
        if (s.kind != "audio") continue;
        AudioPlugin* pl = audio(s.id);
        if (!pl) continue;
        if (p.settings.surround()) {
            if (pl->channels() != 6 && !surroundTried_.count(s.id)) {
                surroundTried_.insert(s.id);
                if (pl->setChannels(6)) logf("audio effect %s: processes the six channels of the 5.1 master", s.name.c_str());
            }
            if (pl->channels() != 6) prepareAudioExtras(s, p);
        } else {
            surroundTried_.erase(s.id);
            if (pl->channels() != 2) pl->setChannels(2);
        }
    }
}

bool FxRuntime::prepareSome(const Project& p, size_t& next, double budgetMs, bool video, bool audio)
{
    if (next == 0) freeRetired();
    const auto start = std::chrono::steady_clock::now();
    const double fps = p.settings.fps();
    const double projectFrames = std::max(1.0, p.length() * fps);
    // the slots in prepare()'s order, counted; those from 'next' on are made while there is time
    size_t k = 0;
    bool stop = false;
    auto one = [&](const FxSlot& s, const char* role, double frames) {
        const bool isVideo = s.kind == "ofx", isAudio = s.kind == "audio";
        if (!isVideo && !isAudio) return;
        if (k++ < next || stop) return;
        if (isVideo && video) prepareVideo(s, p, role, frames);
        else if (isAudio && audio) prepareAudio(s, p);
        ++next;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms >= budgetMs) stop = true;
    };
    auto chain = [&](const FxChain& c, const char* role, double frames) {
        for (const auto& s : c) one(s, role, frames);
    };
    for (const auto& m : p.media)
        if (m.generated && m.generator.kind == "ofx") one(m.generator, "generator", 1.0e6);
    for (const auto& t : p.tracks) {
        chain(t.fx, "track", projectFrames);
        for (const auto& e : t.events) {
            chain(e.fx, "event", std::max(1.0, e.length * fps));
            if (e.transition.kind == "ofx") one(e.transition, "transition", std::max(1.0, e.length * fps));
        }
    }
    chain(p.videoOut, "project", projectFrames);
    chain(p.audioOut, "project", projectFrames);
    for (const auto& b : p.buses) chain(b.fx, "project", projectFrames);
    if (next < k) return false;
    if (audio) prepareSurround(p);
    return true;
}

void FxRuntime::releaseExcept(const std::set<uint64_t>& keep)
{
    std::vector<VideoFx> v;
    std::vector<AudioFx> a;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = video_.begin(); it != video_.end();) {
            if (!keep.count(it->first)) { v.push_back(std::move(it->second)); errors_.erase(it->first); fallbacks_.erase(it->first); it = video_.erase(it); }
            else ++it;
        }
        for (auto it = audio_.begin(); it != audio_.end();) {
            if (!keep.count(it->first)) {
                a.push_back(std::move(it->second));
                errors_.erase(it->first);
                surroundTried_.erase(it->first);
                it = audio_.erase(it);
            }
            else ++it;
        }
    }
    retire(std::move(v), std::move(a));
}

void FxRuntime::captureStates(Project& p)
{
    p.forEachFx([&](FxSlot& s) {
        if (s.kind == "ofx") {
            if (ofx::Instance* inst = video(s.id)) s.state = inst->saveState();
        } else if (s.kind == "audio") {
            if (AudioPlugin* pl = audio(s.id)) s.state = Json(pl->saveState());
        }
    });
}

}  // namespace reno
