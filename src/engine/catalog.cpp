#include "catalog.h"
#include "fxnames.h"
#include "../core/process.h"
#include "../ofx/ofxhost.h"
#include "../ofx/builtins.h"
#include <filesystem>
#include <algorithm>
#include <set>

namespace reno {

Catalog& Catalog::get()
{
    static Catalog c;
    static std::once_flag once;
    std::call_once(once, [] {
        // before any scan or cache: Reno's built-in plug-ins
        std::lock_guard<std::mutex> lk(c.mutex_);
        c.rebuildLocked();
    });
    return c;
}

std::string catalogScanVersion()
{
    // 2: OpenFX effects' labels from their bundles' resource files, VEGAS Pro 18's host
    // 3: 32-bit VST2 plug-ins are run through reno_vst32 (audio files that did not work are tried again)
    const ofx::HostConfig& hc = ofx::Host::get().config;
    return "3|" + hc.hostName + (hc.vegasExtensions ? "|vegas" : "|plain");
}

static std::mutex gProbeMutex;
static std::string gProbeOverride;
void setProbeExeOverride(const std::string& path)
{
    std::lock_guard<std::mutex> lk(gProbeMutex);
    gProbeOverride = path;
}

std::string defaultProbeExe()
{
    {
        std::lock_guard<std::mutex> lk(gProbeMutex);
        if (!gProbeOverride.empty() && fileExists(gProbeOverride)) return gProbeOverride;
    }
#ifdef _WIN32
    return pathJoin(pathDir(selfExePath()), "reno_cli.exe");
#else
    return pathJoin(pathDir(selfExePath()), "reno_cli");
#endif
}

std::vector<VideoFxEntry> Catalog::video() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return video_;
}

std::vector<AudioFxEntry> Catalog::audio() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return audio_;
}

bool Catalog::empty() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& e : video_) if (!e.builtin) return false;
    for (const auto& e : audio_) if (e.desc.format != "BUILTIN") return false;
    return true;
}

bool Catalog::findVideo(const std::string& id, VideoFxEntry* out) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    const VideoFxEntry* best = nullptr;
    for (const auto& e : video_)
        if (e.id == id && (!best || (e.ok && !best->ok) || (e.ok == best->ok && e.builtin != best->builtin && !e.builtin) ||
                           (e.ok == best->ok && e.builtin == best->builtin && e.versionMajor > best->versionMajor))) best = &e;
    if (!best) return false;
    if (out) *out = *best;
    return true;
}

static std::string cachePath() { return pathJoin(userDataDir(), "plugins.json"); }

static std::string fileStamp(const std::string& path)
{
    std::error_code ec;
#ifdef _WIN32
    std::filesystem::path p(widen(path));
#else
    std::filesystem::path p(path);
#endif
    auto size = std::filesystem::file_size(p, ec);
    if (ec) return std::string();
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return std::string();
    return strfmt("%llu:%lld", (unsigned long long)size, (long long)t.time_since_epoch().count());
}

static bool hasContext(const Json& p, const char* name)
{
    for (const auto& c : p.get("contexts").items()) if (c.asString() == name) return true;
    return false;
}

void Catalog::rebuildLocked()
{
    video_.clear();
    audio_.clear();
    for (const auto& f : files_.members()) {
        const Json& r = f.second.get("result");
        const std::string kind = f.second.get("kind").asString();
        if (kind == "ofx") {
            for (const auto& p : r.get("plugins").items()) {
                VideoFxEntry e;
                e.id = p.get("id").asString();
                e.label = p.get("label").asString(e.id);
                // no label of its own: VEGAS's label for it, else the identifier made readable
                if (e.label.empty() || e.label == e.id) {
                    e.label = vegasStockFxLabel(e.id);
                    if (e.label.empty()) e.label = readableFxId(e.id);
                }
                e.group = p.get("group").asString();
                e.path = f.first;
                e.versionMajor = p.get("versionMajor").asInt();
                e.versionMinor = p.get("versionMinor").asInt();
                e.filter = hasContext(p, "OfxImageEffectContextFilter") || hasContext(p, "OfxImageEffectContextGeneral");
                e.generator = hasContext(p, "OfxImageEffectContextGenerator");
                e.transition = hasContext(p, "OfxImageEffectContextTransition");
                e.ok = p.get("ok").asBool();
                e.error = p.get("error").asString();
                video_.push_back(e);
            }
        } else if (kind == "audio") {
            for (const auto& p : r.get("plugins").items()) {
                AudioFxEntry e;
                e.desc = AudioPluginDesc::fromJson(p);
                e.ok = p.get("ok").asBool(true);
                e.error = p.get("error").asString();
                if (!e.desc.isInstrument) audio_.push_back(e);
            }
        } else if (kind == "dx") {
            for (const auto& p : r.items()) {
                AudioFxEntry e;
                e.desc = AudioPluginDesc::fromJson(p);
                e.ok = true;
                audio_.push_back(e);
            }
        }
    }
    // Reno's built-in plug-ins (VEGAS's stock generators and most-used effects).
    for (const auto& b : ofx::builtinInfos()) {
        bool real = false;
        for (const auto& e : video_) if (e.id == b.id && e.ok) real = true;
        if (real) continue;
        video_.erase(std::remove_if(video_.begin(), video_.end(), [&](const VideoFxEntry& e) { return e.id == b.id; }), video_.end());
        VideoFxEntry e;
        e.id = b.id;
        e.label = b.label;
        e.group = b.group;
        e.path = ofx::kBuiltinBinaryPath;
        e.versionMajor = 1;
        e.filter = b.filter;
        e.generator = b.generator;
        e.transition = b.transition;
        e.ok = true;
        e.builtin = true;
        video_.push_back(e);
    }
    // Reno's built-in audio effects (stand-ins for VEGAS's DirectX ones): always there
    for (const AudioPluginDesc& d : builtinAudioPlugins()) {
        AudioFxEntry e;
        e.desc = d;
        e.ok = true;
        audio_.push_back(e);
    }
    std::sort(video_.begin(), video_.end(), [](const VideoFxEntry& a, const VideoFxEntry& b) { return toLower(a.label) < toLower(b.label); });
    std::sort(audio_.begin(), audio_.end(), [](const AudioFxEntry& a, const AudioFxEntry& b) { return toLower(a.desc.name) < toLower(b.desc.name); });
}

bool Catalog::loadCache()
{
    std::string text;
    if (!readFile(cachePath(), text)) return false;
    Json j;
    if (!Json::parse(text, j) || !j.isObject()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    files_ = j.get("files");
    if (!files_.isObject()) files_ = Json::object();
    scanVersion_ = j.get("scanVersion").asString();
    extraVideoDirs.clear();
    extraAudioDirs.clear();
    for (const auto& d : j.get("extraVideoDirs").items()) extraVideoDirs.push_back(d.asString());
    for (const auto& d : j.get("extraAudioDirs").items()) extraAudioDirs.push_back(d.asString());
    rebuildLocked();
    return true;
}

void Catalog::saveCache() const
{
    Json j = Json::object();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        j["files"] = files_;
        j["scanVersion"] = scanVersion_;
        Json v = Json::array(), a = Json::array();
        for (const auto& d : extraVideoDirs) v.push(d);
        for (const auto& d : extraAudioDirs) a.push(d);
        j["extraVideoDirs"] = v;
        j["extraAudioDirs"] = a;
    }
    writeFile(cachePath(), j.dump(1));
}

void Catalog::scan(const std::string& probeExe, bool full, const std::function<void(const std::string&, int, int)>& progress, std::atomic<bool>* cancel)
{
    std::vector<std::string> vdirs = ofx::Host::defaultSearchDirs(), adirs = audioPluginSearchDirs();
    Json old;
    bool olderList = false;
    const std::string version = catalogScanVersion();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        vdirs.insert(vdirs.end(), extraVideoDirs.begin(), extraVideoDirs.end());
        adirs.insert(adirs.end(), extraAudioDirs.begin(), extraAudioDirs.end());
        old = files_;
        // a list made another way (an older Reno, another host name): every OpenFX file is tried again
        if (scanVersion_ != version) olderList = true;
    }
    struct Job { std::string path, kind; };
    std::vector<Job> jobs;
    for (const auto& f : ofx::Host::findBinaries(vdirs)) jobs.push_back({f, "ofx"});
    for (const auto& f : findAudioPluginFiles(adirs)) jobs.push_back({f, "audio"});
    jobs.push_back({"<DirectX audio effects>", "dx"});

    const std::string tmp = pathJoin(userDataDir(), strfmt("probe_%llu.json", (unsigned long long)std::hash<std::string>()(selfExePath())));
    Json fresh = Json::object();
    int done = 0;
    for (const auto& job : jobs) {
        if (cancel && *cancel) break;
        if (progress) progress(job.kind == "dx" ? job.path : pathFileName(job.path), done, (int)jobs.size());
        ++done;
        const std::string stamp = job.kind == "dx" ? std::string("dx") : fileStamp(job.path);
        const Json& was = old.get(job.path);
        if (!full && job.kind != "dx" && !(olderList && job.kind == "ofx") &&
            !(olderList && job.kind == "audio" && was.isObject() && !was.get("result").get("ok").asBool()) && was.isObject() && was.get("stamp").asString() == stamp) {
            fresh[job.path] = was;
            continue;
        }
        writeFile(tmp, "");
        std::vector<std::string> args;
        if (job.kind == "ofx") {
            // the child tells effects what this Reno tells them (Preferences), so the scan sees what using them will
            const ofx::HostConfig& hc = ofx::Host::get().config;
            args = {"ofxprobe", "--out", tmp, "--no-render"};
            if (!hc.vegasExtensions) args.push_back("--plain-host");
            args.insert(args.end(), {"--host-name", hc.hostName, job.path});
        }
        else if (job.kind == "audio") args = {"audioprobe", "--out", tmp, "--describe-only", job.path};
        else args = {"dxprobe", "--out", tmp};
        ProcessResult pr = runProcess(probeExe, args, job.kind == "ofx" ? 180 : 90, cancel);
        if (pr.stopped) break;
        std::string text;
        readFile(tmp, text);
        Json out;
        if (!text.empty()) Json::parse(text, out);
        Json entry = Json::object();
        entry["stamp"] = stamp;
        entry["kind"] = job.kind;
        if (pr.crashed()) entry["crashed"] = true;
        if (job.kind == "dx") entry["result"] = out.isArray() ? out : Json::array();
        else {
            Json r = out.at(0);
            if (!r.isObject()) { r = Json::object(); r["ok"] = false; }
            if (!pr.started || pr.timedOut || pr.exitCode != 0) {
                // whatever was being tried when the child died is the one to blame; mark every effect that has no verdict
                r["processProblem"] = pr.describe();
            }
            entry["result"] = r;
        }
        fresh[job.path] = entry;
    }
    std::error_code ec;
#ifdef _WIN32
    std::filesystem::remove(std::filesystem::path(widen(tmp)), ec);
#else
    std::filesystem::remove(std::filesystem::path(tmp), ec);
#endif
    if (cancel && *cancel) {
        // keep what was known about the files not reached
        for (const auto& m : old.members()) if (!fresh.has(m.first)) fresh[m.first] = m.second;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        files_ = fresh;
        if (!(cancel && *cancel)) scanVersion_ = version;
        rebuildLocked();
    }
    saveCache();
    if (progress) progress("", (int)jobs.size(), (int)jobs.size());
}

bool Catalog::changedSinceScan(std::string* what) const
{
    std::vector<std::string> vdirs = ofx::Host::defaultSearchDirs(), adirs = audioPluginSearchDirs();
    Json known;
    bool olderList = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        vdirs.insert(vdirs.end(), extraVideoDirs.begin(), extraVideoDirs.end());
        adirs.insert(adirs.end(), extraAudioDirs.begin(), extraAudioDirs.end());
        known = files_;
        olderList = scanVersion_ != catalogScanVersion();
    }
    auto say = [&](const std::string& path) {
        if (what) *what = path;
        return true;
    };
    if (!known.isObject() || known.members().empty()) return say(std::string());     // never looked
    if (olderList) return say(std::string());                                         // made by an older Reno
    std::set<std::string> seen;
    std::vector<std::string> files = ofx::Host::findBinaries(vdirs);
    for (const auto& f : findAudioPluginFiles(adirs)) files.push_back(f);
    for (const auto& f : files) {
        seen.insert(f);
        const Json& was = known.get(f);
        if (!was.isObject() || was.get("stamp").asString() != fileStamp(f)) return say(f);
    }
    // a plug-in file taken away (its effects are still listed)
    for (const auto& m : known.members())
        if (m.second.get("kind").asString() != "dx" && !seen.count(m.first)) return say(m.first);
    return false;
}

bool Catalog::crashedWhenTried(const std::string& path) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return files_.get(path).get("crashed").asBool();
}

// ------------------------------------------------------------------------------------------ ScanTask

ScanTask& ScanTask::get()
{
    Catalog::get();
    static ScanTask t;
    return t;
}

ScanTask::~ScanTask()
{
    cancel();
    join();
}

bool ScanTask::start(const std::string& probeExe, bool full, bool onlyIfChanged)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (state_.running) return false;
    if (thread_.joinable()) thread_.join();
    const int serial = state_.serial + 1;
    state_ = State();
    state_.serial = serial;
    state_.started = state_.running = true;
    cancel_.store(false);
    thread_ = std::thread([this, probeExe, full, onlyIfChanged] {
        std::string failure;
        bool lookedOnly = false;
        try {
            if (onlyIfChanged && !Catalog::get().changedSinceScan()) lookedOnly = true;
            else
                Catalog::get().scan(probeExe, full,
                                    [this](const std::string& what, int done, int total) {
                                        std::lock_guard<std::mutex> lk2(mutex_);
                                        state_.what = what;
                                        state_.done = done;
                                        state_.total = total;
                                    },
                                    &cancel_);
        } catch (const std::exception& e) {
            failure = e.what();
        } catch (...) {
            failure = "an unexpected error";
        }
        std::lock_guard<std::mutex> lk2(mutex_);
        state_.running = false;
        state_.finished = true;
        state_.lookedOnly = lookedOnly;
        state_.cancelled = cancel_.load();
        state_.failure = failure;
        state_.what.clear();
    });
    return true;
}

void ScanTask::cancel() { cancel_.store(true); }

void ScanTask::join()
{
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        t.swap(thread_);
    }
    if (t.joinable()) t.join();
}

ScanTask::State ScanTask::state() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return state_;
}

void Catalog::scanInProcess(const std::vector<std::string>& ofxDirs, const std::vector<std::string>& audioFiles)
{
    Json fresh = Json::object();
    for (const auto& f : ofx::Host::findBinaries(ofxDirs)) {
        Json r = Json::object();
        r["path"] = f;
        std::string err;
        ofx::Binary* bin = ofx::Host::get().loadBinary(f, &err);
        Json plugins = Json::array();
        if (bin) {
            for (auto& pl : bin->plugins) {
                Json j = Json::object();
                std::string e;
                const bool ok = pl->load(&e);
                j["id"] = pl->info.id;
                j["label"] = pl->info.label;
                j["group"] = pl->info.grouping;
                j["versionMajor"] = pl->info.versionMajor;
                j["versionMinor"] = pl->info.versionMinor;
                Json ctx = Json::array();
                for (const auto& c : pl->info.contexts) ctx.push(c);
                j["contexts"] = ctx;
                j["ok"] = ok;
                if (!ok) j["error"] = e;
                plugins.push(j);
            }
        }
        r["ok"] = bin != nullptr;
        if (!bin) r["error"] = err;
        r["plugins"] = plugins;
        Json entry = Json::object();
        entry["stamp"] = fileStamp(f);
        entry["kind"] = "ofx";
        entry["result"] = r;
        fresh[f] = entry;
    }
    for (const auto& f : audioFiles) {
        Json r = Json::object();
        std::string err;
        Json plugins = Json::array();
        for (const auto& d : describeAudioPluginFile(f, &err)) {
            Json j = d.toJson();
            j["ok"] = true;
            plugins.push(j);
        }
        r["plugins"] = plugins;
        Json entry = Json::object();
        entry["stamp"] = fileStamp(f);
        entry["kind"] = "audio";
        entry["result"] = r;
        fresh[f] = entry;
    }
    const std::string version = catalogScanVersion();
    std::lock_guard<std::mutex> lk(mutex_);
    files_ = fresh;
    scanVersion_ = version;
    rebuildLocked();
}

}  // namespace reno
