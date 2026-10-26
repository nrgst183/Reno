// The effect log (see fxlog.h).
#include "fxlog.h"
#include "catalog.h"
#include "fxnames.h"
#include "../core/base.h"
#include "../core/process.h"
#include "../ofx/ofxhost.h"
#include <ctime>
#include <map>
#include <set>

namespace reno {
namespace {

struct Use {
    const FxSlot* slot;
    std::string role;
};

// Every effect slot of the project with the place it is in.
void collect(const Project& p, std::vector<Use>& out)
{
    for (const auto& m : p.media)
        if (m.generated) out.push_back({&m.generator, "generator"});
    for (const auto& t : p.tracks) {
        const char* kind = t.type == TrackType::Video ? "video track" : "audio track";
        for (const auto& f : t.fx) out.push_back({&f, kind});
        for (const Event& e : t.events) {
            for (const auto& f : e.fx) out.push_back({&f, std::string(t.type == TrackType::Video ? "video event" : "audio event") + (f.beforePanCrop ? " (before pan/crop)" : "")});
            if (!e.transition.kind.empty()) out.push_back({&e.transition, "transition"});
        }
    }
    for (const auto& b : p.buses)
        for (const auto& f : b.fx) out.push_back({&f, "bus"});
    for (const auto& f : p.videoOut) out.push_back({&f, "project video output"});
    for (const auto& f : p.audioOut) out.push_back({&f, "master"});
}

std::string countsText(const std::map<std::string, int>& roles)
{
    std::string s;
    for (const auto& r : roles) s += (s.empty() ? "" : ", ") + std::to_string(r.second) + " " + r.first;
    return s;
}

// One effect identifier's lines.
struct Entry {
    std::string name;
    std::map<std::string, int> roles;
    int uses = 0, made = 0, failed = 0, bypassed = 0;
    std::map<std::string, int> errors;
    std::map<std::string, int> fallbacks;                       // why a plug-in file was not used and Reno's own built-in was
    std::map<std::string, std::pair<int, ofx::Plugin*>> origins;    // where the instances come from
};

}  // namespace

std::string fxLogText(const Project& p, const FxRuntime& rt, const std::string& projectPath)
{
    std::vector<Use> uses;
    collect(p, uses);
    // video: by identifier; audio: by plug-in key
    std::map<std::string, Entry> video, audio, other;
    for (const Use& u : uses) {
        const FxSlot& s = *u.slot;
        const bool isAudio = s.kind == "audio";
        std::map<std::string, Entry>& into = s.kind == "ofx" ? video : (isAudio ? audio : other);
        const std::string key = s.kind == "ofx" ? s.pluginId : (isAudio ? s.audio.key() : s.kind + ":" + s.pluginId);
        Entry& e = into[key];
        if (e.name.empty()) e.name = isAudio ? s.name : fxSlotLabel(s);
        ++e.uses;
        ++e.roles[u.role];
        if (s.bypass) ++e.bypassed;
        const bool made = s.kind == "ofx" ? rt.video(s.id) != nullptr : (isAudio ? rt.audio(s.id) != nullptr : true);
        if (made) {
            ++e.made;
            if (s.kind == "ofx") {
                ofx::Instance* inst = rt.video(s.id);
                ofx::Plugin* pl = inst ? inst->plugin : nullptr;
                const bool builtin = pl && pl->binary && pl->binary->builtin;
                auto& o = e.origins[pl ? (builtin ? std::string("built-in") : (pl->binary ? pl->binary->path : pl->info.binaryPath)) : std::string("?")];
                ++o.first;
                o.second = pl;
                const std::string fb = rt.fallback(s.id);
                if (!fb.empty()) ++e.fallbacks[fb];
            }
        } else {
            ++e.failed;
            std::string why = rt.error(s.id);
            ++e.errors[why.empty() ? std::string("no instance was made (no reason was recorded)") : why];
        }
    }

    std::string out;
    {
        char when[64] = "";
        const std::time_t now = std::time(nullptr);
        std::tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
        out += strfmt("Reno effect log, written %s\n", when);
    }
    out += "Program: " + selfExePath() + "\n";
    out += "Project: " + (projectPath.empty() ? std::string("(none)") : projectPath) + strfmt("  (%dx%d, %.3f fps)\n", p.settings.width, p.settings.height, p.settings.fps());
    int problems = 0;
    for (const auto* m : {&video, &audio, &other})
        for (const auto& kv : *m) problems += kv.second.failed > 0 ? 1 : 0;
    out += strfmt("%zu video effect%s, %zu audio effect%s, %zu other; %zu effect slots in all; %d effect%s with problems\n\n", video.size(), video.size() == 1 ? "" : "s",
                  audio.size(), audio.size() == 1 ? "" : "s", other.size(), uses.size(), problems, problems == 1 ? "" : "s");

    out += "VIDEO EFFECTS (OpenFX)\n";
    if (video.empty()) out += "  (none)\n";
    for (const auto& kv : video) {
        const Entry& e = kv.second;
        out += "\n" + kv.first + "  \"" + e.name + "\"\n";
        out += strfmt("    used %d time%s: %s%s\n", e.uses, e.uses == 1 ? "" : "s", countsText(e.roles).c_str(), e.bypassed ? strfmt(" (%d bypassed)", e.bypassed).c_str() : "");
        // where the instances come from
        for (const auto& o : e.origins) {
            if (o.first == "built-in") out += strfmt("    from: built-in (Reno's own reproduction), %d instance%s\n", o.second.first, o.second.first == 1 ? "" : "s");
            else {
                out += strfmt("    from: plug-in file %s, %d instance%s\n", o.first.c_str(), o.second.first, o.second.first == 1 ? "" : "s");
                if (const ofx::Plugin* pl = o.second.second) {
                    out += strfmt("    plug-in: multi-resolution %s, render thread safety %s%s\n", pl->info.multiResolution ? "yes" : "no",
                                  pl->safety >= 2 ? "fully safe" : (pl->safety == 1 ? "instance safe" : "unsafe (one render at a time)"), pl->unusable ? ", cannot run here" : "");
                    if (!pl->error.empty()) out += "    plug-in message: " + pl->error + "\n";
                    if (!pl->missingFeature.empty()) out += "    host feature it missed: " + pl->missingFeature + "\n";
                }
            }
        }
        for (const auto& fb : e.fallbacks) out += strfmt("    plug-in file not used (%d time%s), built-in version used instead: %s\n", fb.second, fb.second == 1 ? "" : "s", fb.first.c_str());
        if (e.made < e.uses) {
            // some instances could not be made: say where the effect is looked for and what the catalog knows
            std::string err;
            ofx::Plugin* pl = findVideoPlugin(kv.first, &err);
            VideoFxEntry ce;
            const bool inCatalog = Catalog::get().findVideo(kv.first, &ce);
            if (pl && pl->binary && pl->binary->builtin) out += "    tried: built-in (Reno's own reproduction)\n";
            else if (pl) out += "    tried: plug-in file " + (pl->binary ? pl->binary->path : pl->info.binaryPath) + "\n";
            else out += "    from: NOT FOUND" + (err.empty() ? std::string() : " - " + err) + "\n";
            if (inCatalog && !ce.builtin) out += "    the catalog lists " + ce.path + (ce.ok ? std::string() : " (not working: " + ce.error + ")") + "\n";
        }
        out += strfmt("    instances made: %d of %d\n", e.made, e.uses);
        for (const auto& er : e.errors) out += strfmt("    ERROR (%d time%s): %s\n", er.second, er.second == 1 ? "" : "s", er.first.c_str());
    }

    out += "\nAUDIO EFFECTS\n";
    if (audio.empty()) out += "  (none)\n";
    for (const auto& kv : audio) {
        const Entry& e = kv.second;
        out += "\n\"" + e.name + "\"  [" + kv.first + "]\n";
        out += strfmt("    used %d time%s: %s%s\n", e.uses, e.uses == 1 ? "" : "s", countsText(e.roles).c_str(), e.bypassed ? strfmt(" (%d bypassed)", e.bypassed).c_str() : "");
        const std::string fmt = kv.first.substr(0, kv.first.find('|'));
        std::string path = kv.first.substr(std::min(kv.first.size(), kv.first.find('|') + 1));
        path = path.substr(0, path.find('|'));
        out += "    from: " + fmt + (path.empty() ? std::string(" (installed with Windows)") : " plug-in file " + path) + "\n";
        out += strfmt("    instances made: %d of %d\n", e.made, e.uses);
        for (const auto& er : e.errors) out += strfmt("    ERROR (%d time%s): %s\n", er.second, er.second == 1 ? "" : "s", er.first.c_str());
    }
    if (!other.empty()) {
        out += "\nOTHER EFFECTS\n";
        for (const auto& kv : other) out += strfmt("\n%s  \"%s\"\n    used %d time%s: %s\n    from: built into Reno\n", kv.first.c_str(), kv.second.name.c_str(), kv.second.uses, kv.second.uses == 1 ? "" : "s", countsText(kv.second.roles).c_str());
    }
    out += "\n(Effects of nested projects used as media are not listed here.)\n";
    return out;
}

bool writeFxLog(const Project& p, const FxRuntime& rt, const std::string& projectPath, const std::string& dir)
{
    try {
        std::string d = dir;
        if (d.empty()) d = pathDir(selfExePath());
        if (d.empty()) return false;
        return writeFile(pathJoin(d, "fx-log.txt"), fxLogText(p, rt, projectPath));
    } catch (...) {
        return false;
    }
}

}  // namespace reno
