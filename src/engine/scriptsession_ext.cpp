// The script's view of the project's HDR and stereoscopic 3D settings, of track groups and of media bins
// (ScriptSession's ext* functions; scriptsession.cpp calls them first).
#include "scriptsession.h"
#include <algorithm>
#include <cmath>

namespace reno {

namespace {

const char* const kStereoNames[] = {"off", "sbshalf", "sbsfull", "tbhalf", "tbfull", "anaglyph-rc", "anaglyph-ab", "anaglyph-gm",
                                    "line", "checker", "left", "right", "blend", "difference"};
const int kStereoCount = 14;       // VEGAS's Stereo3DOutputMode 0..13 (14, LeftAndRight, is not offered)

int64_t flag(bool b) { return b ? 1 : 0; }

std::string parentOf(const std::string& path)
{
    const size_t k = path.rfind('/');
    return k == std::string::npos ? std::string() : path.substr(0, k);
}
std::string leafOf(const std::string& path)
{
    const size_t k = path.rfind('/');
    return k == std::string::npos ? path : path.substr(k + 1);
}
bool inside(const std::string& path, const std::string& bin)      // path is bin or below it
{
    return path == bin || (path.size() > bin.size() && path.compare(0, bin.size(), bin) == 0 && path[bin.size()] == '/');
}

}  // namespace

std::vector<uint64_t> ScriptSession::trackGroupIds() const
{
    std::vector<uint64_t> ids;
    for (const Track& t : project.tracks)
        if (t.trackGroup && std::find(ids.begin(), ids.end(), t.trackGroup) == ids.end()) ids.push_back(t.trackGroup);
    return ids;
}

std::vector<std::string> ScriptSession::binPaths() const
{
    std::set<std::string> all;
    auto addWithParents = [&](std::string p) {
        while (!p.empty()) {
            all.insert(p);
            p = parentOf(p);
        }
    };
    for (const std::string& b : project.mediaBins) addWithParents(b);
    for (const MediaItem& m : project.media) addWithParents(m.bin);
    return std::vector<std::string>(all.begin(), all.end());
}

bool ScriptSession::binExists(const std::string& path) const
{
    if (path.empty()) return true;
    const std::vector<std::string> all = binPaths();
    return std::find(all.begin(), all.end(), path) != all.end();
}

const std::vector<ScriptRenderer>& ScriptSession::renderersOf()
{
    if (!renderersRead_) {
        renderersRead_ = true;
        if (host_.renderers) renderers_ = host_.renderers();
    }
    return renderers_;
}

const std::vector<ScriptPreset>& ScriptSession::presetsOf(size_t pluginIndex)
{
    auto it = presets_.find(pluginIndex);
    if (it != presets_.end()) return it->second;
    std::vector<ScriptPreset> v;
    if (host_.presets && pluginIndex < host_.plugins.size()) {
        const ScriptPlugin& p = host_.plugins[pluginIndex];
        if (p.type != ScriptPlugin::Audio)
            v = host_.presets(p.id, p.type == ScriptPlugin::Transition ? "Transition" : p.type == ScriptPlugin::Generator ? "Generator" : "Filter");
    }
    return presets_[pluginIndex] = v;
}

int ScriptSession::extGetI(const Desc& d, int prop, int64_t* out)
{
    if (d.kind == RK_APP) {
        const ProjectSettings& st = project.settings;
        switch (prop) {
        case RP_HDR_MODE: *out = st.hdrMode == "hdr10" ? 0 : st.hdrMode == "hlg" ? 1 : 2; return RS_OK;
        case RP_STEREO_MODE: {
            *out = 0;
            for (int k = 0; k < kStereoCount; ++k) if (st.stereoMode == kStereoNames[k]) *out = k;
            return RS_OK;
        }
        case RP_STEREO_SWAP: *out = flag(st.stereoSwap); return RS_OK;
        case RP_STEREO_CROSSTALK_RENDER: *out = flag(st.crosstalkInRender); return RS_OK;
        }
        return kNotMine;
    }
    if (d.kind == RK_TRACK && prop == RP_TRACK_GROUP) {
        const Track* t = project.findTrack(d.a);
        if (!t) return fail("the track is no longer in the project");
        *out = t->trackGroup ? (int64_t)handle({RK_TRACKGROUP, t->trackGroup, 0, 0, ""}) : 0;
        return RS_OK;
    }
    if (d.kind == RK_MEDIA && prop == RP_MEDIA_BIN) {
        const MediaItem* m = project.findMedia(d.a);
        if (!m) return fail("the media is no longer in the project");
        *out = (int64_t)handle({RK_BIN, 0, 0, 0, m->bin});
        return RS_OK;
    }
    if (d.kind == RK_TRACKGROUP) {
        const auto ids = trackGroupIds();
        const auto at = std::find(ids.begin(), ids.end(), d.a);
        if (prop == RP_IS_VALID) { *out = flag(at != ids.end()); return RS_OK; }
        if (at == ids.end()) return fail("the track group no longer exists");
        bool all = true;
        switch (prop) {
        case RP_INDEX: *out = at - ids.begin(); return RS_OK;
        case RP_MUTE:
            for (const Track& t : project.tracks) if (t.trackGroup == d.a && !t.mute) all = false;
            *out = flag(all);
            return RS_OK;
        case RP_SOLO:
            for (const Track& t : project.tracks) if (t.trackGroup == d.a && !t.solo) all = false;
            *out = flag(all);
            return RS_OK;
        }
        return unsupported();
    }
    if (d.kind == RK_RENDERER || d.kind == RK_RTEMPLATE) {
        const auto& rs = renderersOf();
        const ScriptRenderer* r = d.a < rs.size() ? &rs[(size_t)d.a] : nullptr;
        const ScriptRenderTemplate* t = r && d.kind == RK_RTEMPLATE && d.c >= 0 && d.c < (int64_t)r->templates.size() ? &r->templates[(size_t)d.c] : nullptr;
        if (prop == RP_IS_VALID) { *out = flag(d.kind == RK_RENDERER ? r != nullptr : t != nullptr); return RS_OK; }
        if (!r || (d.kind == RK_RTEMPLATE && !t)) return fail("the render format or template no longer exists");
        switch (prop) {
        case RP_INDEX: *out = d.kind == RK_RENDERER ? (int64_t)d.a : d.c; return RS_OK;
        case RP_RT_HAS_VIDEO: *out = flag(t ? t->video : r->hasVideo); return RS_OK;
        case RP_RT_HAS_AUDIO: *out = flag(t ? t->audio : r->hasAudio); return RS_OK;
        }
        if (t) {
            switch (prop) {
            case RP_RT_WIDTH: *out = t->width; return RS_OK;
            case RP_RT_HEIGHT: *out = t->height; return RS_OK;
            case RP_RT_SAMPLE_RATE: *out = t->sampleRate; return RS_OK;
            case RP_RT_CHANNELS: *out = t->channels; return RS_OK;
            case RP_RT_BITS: *out = t->bits; return RS_OK;
            case RP_RT_VIDEO_KBPS: *out = t->videoKbps; return RS_OK;
            case RP_RT_AUDIO_KBPS: *out = t->audioKbps; return RS_OK;
            case RP_RT_RENDERER: *out = (int64_t)handle({RK_RENDERER, d.a, 0, 0, ""}); return RS_OK;
            }
        }
        return unsupported();
    }
    if (d.kind == RK_PRESET) {
        const auto& v = presetsOf((size_t)d.a);
        if (prop == RP_IS_VALID) { *out = flag(d.c >= 0 && d.c < (int64_t)v.size() && v[(size_t)d.c].name == d.s); return RS_OK; }
        if (prop == RP_INDEX) { *out = d.c; return RS_OK; }
        return unsupported();
    }
    if (d.kind == RK_BIN) {
        if (prop == RP_IS_VALID) { *out = flag(binExists(d.s)); return RS_OK; }
        if (!binExists(d.s)) return fail("the media bin no longer exists");
        switch (prop) {
        case RP_PARENT:
            if (d.s.empty()) { *out = 0; return RS_OK; }
            *out = (int64_t)handle({RK_BIN, 0, 0, 0, parentOf(d.s)});
            return RS_OK;
        case RP_INDEX: *out = d.s.empty() ? -1 : 0; return RS_OK;
        }
        return unsupported();
    }
    return kNotMine;
}

static double* motionField(MotionKey& k, int prop)
{
    switch (prop) {
    case RP_KEY_Z: return &k.z;
    case RP_KEY_DEPTH: return &k.d;
    case RP_KEY_ROT_X: return &k.rotX;
    case RP_KEY_ROT_Y: return &k.rotY;
    case RP_KEY_OFF_X: return &k.offX;
    case RP_KEY_OFF_Y: return &k.offY;
    case RP_KEY_OFF_Z: return &k.offZ;
    case RP_KEY_ORI_X: return &k.oriX;
    case RP_KEY_ORI_Y: return &k.oriY;
    case RP_KEY_ORI_Z: return &k.oriZ;
    }
    return nullptr;
}

int ScriptSession::extGetD(const Desc& d, int prop, double* out)
{
    if (d.kind == RK_RTEMPLATE && (prop == RP_RT_FPS || prop == RP_RT_PAR)) {
        const auto& rs = renderersOf();
        if (d.a >= rs.size() || d.c < 0 || d.c >= (int64_t)rs[(size_t)d.a].templates.size()) return fail("the render template no longer exists");
        const ScriptRenderTemplate& t = rs[(size_t)d.a].templates[(size_t)d.c];
        *out = prop == RP_RT_PAR ? t.par : t.fpsNum > 0 ? (double)t.fpsNum / std::max(1, t.fpsDen) : 0.0;
        return RS_OK;
    }
    if (d.kind == RK_MOTION && prop >= RP_KEY_Z && prop <= RP_KEY_ORI_Z) {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        if (d.c < 0 || d.c >= (int64_t)t->motion.size()) return fail("the track motion key is no longer there");
        *out = *motionField(t->motion[(size_t)d.c], prop);
        return RS_OK;
    }
    if (d.kind == RK_APP && prop == RP_STEREO_CROSSTALK) {
        *out = project.settings.crosstalk;
        return RS_OK;
    }
    return kNotMine;
}

int ScriptSession::extGetS(const Desc& d, int prop, std::string* out)
{
    if ((d.kind == RK_RENDERER || d.kind == RK_RTEMPLATE) && (prop == RP_NAME || prop == RP_FILE_EXT || prop == RP_DESCRIPTION)) {
        const auto& rs = renderersOf();
        if (d.a >= rs.size()) return fail("the render format no longer exists");
        const ScriptRenderer& r = rs[(size_t)d.a];
        if (d.kind == RK_RENDERER) {
            *out = prop == RP_NAME ? r.name : prop == RP_FILE_EXT ? r.ext : r.description;
            return RS_OK;
        }
        if (d.c < 0 || d.c >= (int64_t)r.templates.size()) return fail("the render template no longer exists");
        const ScriptRenderTemplate& t = r.templates[(size_t)d.c];
        *out = prop == RP_NAME ? t.name : prop == RP_FILE_EXT ? (t.ext.empty() ? r.ext : t.ext) : t.description;
        return RS_OK;
    }
    if (d.kind == RK_PRESET && prop == RP_NAME) {
        *out = d.s;
        return RS_OK;
    }
    if (d.kind == RK_BIN && prop == RP_NAME) {
        if (!binExists(d.s)) return fail("the media bin no longer exists");
        *out = d.s.empty() ? "Project Media" : leafOf(d.s);
        return RS_OK;
    }
    return kNotMine;
}

int ScriptSession::extSetI(const Desc& d, int prop, int64_t v)
{
    if (d.kind == RK_APP) {
        ProjectSettings& st = project.settings;
        switch (prop) {
        case RP_HDR_MODE:
            if (v < 0 || v > 2) return fail("not one of VEGAS's HDR modes");
            st.hdrMode = v == 0 ? "hdr10" : v == 1 ? "hlg" : "off";
            touch();
            return RS_OK;
        case RP_STEREO_MODE:
            if (v == 14) return fail("Reno has no Left and Right output mode");
            if (v < 0 || v >= kStereoCount) return fail("not one of VEGAS's stereoscopic 3D output modes");
            st.stereoMode = kStereoNames[v];
            touch();
            return RS_OK;
        case RP_STEREO_SWAP: st.stereoSwap = v != 0; touch(); return RS_OK;
        case RP_STEREO_CROSSTALK_RENDER: st.crosstalkInRender = v != 0; touch(); return RS_OK;
        }
        return kNotMine;
    }
    if (d.kind == RK_TRACKGROUP && (prop == RP_MUTE || prop == RP_SOLO)) {
        const auto ids = trackGroupIds();
        if (std::find(ids.begin(), ids.end(), d.a) == ids.end()) return fail("the track group no longer exists");
        for (Track& t : project.tracks)
            if (t.trackGroup == d.a) (prop == RP_MUTE ? t.mute : t.solo) = v != 0;
        touch();
        return RS_OK;
    }
    return kNotMine;
}

int ScriptSession::extSetD(const Desc& d, int prop, double v)
{
    if (d.kind == RK_MOTION && prop >= RP_KEY_Z && prop <= RP_KEY_ORI_Z) {
        Track* t = track(d);
        if (!t) return RS_FAILED;
        if (d.c < 0 || d.c >= (int64_t)t->motion.size()) return fail("the track motion key is no longer there");
        if (prop == RP_KEY_DEPTH && v < 0) return fail("depth cannot be negative");
        *motionField(t->motion[(size_t)d.c], prop) = v;
        touch();
        return RS_OK;
    }
    if (d.kind == RK_APP && prop == RP_STEREO_CROSSTALK) {
        if (v < 0 || v > 1) return fail("crosstalk cancelation is from 0 to 1");
        project.settings.crosstalk = v;
        touch();
        return RS_OK;
    }
    return kNotMine;
}

int ScriptSession::extSetS(const Desc& d, int prop, const std::string& v)
{
    if (d.kind == RK_BIN && prop == RP_NAME) {
        if (d.s.empty()) return fail("the Project Media folder cannot be renamed");
        if (!binExists(d.s)) return fail("the media bin no longer exists");
        if (v.empty() || v.find('/') != std::string::npos) return fail("a media bin's name cannot be empty or hold a slash");
        const std::string parent = parentOf(d.s);
        const std::string now = parent.empty() ? v : parent + "/" + v;
        if (now == d.s) return RS_OK;
        if (binExists(now)) return fail("there is a media bin of that name already");
        const std::string old = d.s;
        auto rename = [&](std::string& p) { if (inside(p, old)) p = now + p.substr(old.size()); };
        for (std::string& b : project.mediaBins) rename(b);
        for (MediaItem& m : project.media) rename(m.bin);
        // the script's handles to this bin and the bins in it follow it
        std::map<Desc, uint64_t> moved;
        for (size_t k = 0; k < descs_.size(); ++k)
            if (descs_[k].kind == RK_BIN) rename(descs_[k].s);
        for (size_t k = 0; k < descs_.size(); ++k) moved[descs_[k]] = (uint64_t)k + 1;
        handles_ = moved;
        touch();
        return RS_OK;
    }
    return kNotMine;
}

int ScriptSession::extList(const Desc& d, int which, std::vector<uint64_t>* out)
{
    switch (which) {
    case RL_RENDERERS: {
        if (d.kind != RK_APP) return unsupported();
        const auto& rs = renderersOf();
        for (size_t k = 0; k < rs.size(); ++k) out->push_back(handle({RK_RENDERER, (uint64_t)k, 0, 0, ""}));
        return RS_OK;
    }
    case RL_RENDER_TEMPLATES: {
        if (d.kind != RK_RENDERER) return unsupported();
        const auto& rs = renderersOf();
        if (d.a >= rs.size()) return fail("the render format no longer exists");
        for (size_t k = 0; k < rs[(size_t)d.a].templates.size(); ++k) out->push_back(handle({RK_RTEMPLATE, d.a, 0, (int64_t)k, ""}));
        return RS_OK;
    }
    case RL_PRESETS: {
        int64_t plugin = -1;
        if (d.kind == RK_PLUGIN) plugin = d.c;
        else if (d.kind == RK_FX) {
            FxSlot* slot = fx(d);
            if (!slot) return RS_FAILED;
            for (size_t k = 0; k < host_.plugins.size() && plugin < 0; ++k)
                if (slot->kind == "ofx" && host_.plugins[k].type != ScriptPlugin::Audio && host_.plugins[k].id == slot->pluginId) plugin = (int64_t)k;
        } else return unsupported();
        if (plugin < 0) return RS_OK;
        const auto& v = presetsOf((size_t)plugin);
        for (size_t k = 0; k < v.size(); ++k) out->push_back(handle({RK_PRESET, (uint64_t)plugin, 0, (int64_t)k, v[k].name}));
        return RS_OK;
    }
    case RL_TRACK_GROUPS:
        if (d.kind != RK_APP) return unsupported();
        for (uint64_t g : trackGroupIds()) out->push_back(handle({RK_TRACKGROUP, g, 0, 0, ""}));
        return RS_OK;
    case RL_GROUP_TRACKS:
        if (d.kind != RK_TRACKGROUP) return unsupported();
        for (const Track& t : project.tracks)
            if (t.trackGroup == d.a) out->push_back(handle({RK_TRACK, t.id, 0, 0, ""}));
        return RS_OK;
    case RL_BIN_BINS:
        if (d.kind != RK_BIN) return unsupported();
        if (!binExists(d.s)) return fail("the media bin no longer exists");
        for (const std::string& b : binPaths())
            if (parentOf(b) == d.s) out->push_back(handle({RK_BIN, 0, 0, 0, b}));
        return RS_OK;
    case RL_BIN_MEDIA:
        if (d.kind != RK_BIN) return unsupported();
        if (!binExists(d.s)) return fail("the media bin no longer exists");
        for (const MediaItem& m : project.media)
            if (m.bin == d.s) out->push_back(handle({RK_MEDIA, m.id, 0, 0, ""}));
        return RS_OK;
    }
    return kNotMine;
}

int ScriptSession::extCall(const Desc& d, int op, const std::vector<int64_t>& ia, const std::vector<double>& da, const std::string& s, uint64_t* out, double* dout)
{
    auto I = [&](size_t k) -> int64_t { return k < ia.size() ? ia[k] : 0; };
    switch (op) {
    case RO_GROUP_SELECTED_TRACKS: {
        if (d.kind != RK_APP) return unsupported();
        std::vector<Track*> chosen;
        for (Track& t : project.tracks) if (selectedTracks.count(t.id)) chosen.push_back(&t);
        if (chosen.size() < 2) return fail("two or more tracks must be selected to make a track group");
        const uint64_t g = project.newId();
        for (Track* t : chosen) t->trackGroup = g;
        touch();
        *out = handle({RK_TRACKGROUP, g, 0, 0, ""});
        return RS_OK;
    }
    case RO_TRACKGROUP_ADD: {
        if (d.kind != RK_TRACKGROUP) return unsupported();
        const auto ids = trackGroupIds();
        if (std::find(ids.begin(), ids.end(), d.a) == ids.end()) return fail("the track group no longer exists");
        const Desc* td = desc((uint64_t)I(0));
        if (!td || td->kind != RK_TRACK) return fail("not a track");
        Track* t = track(*td);
        if (!t) return RS_FAILED;
        t->trackGroup = d.a;
        touch();
        return RS_OK;
    }
    case RO_RENDER: {
        if (d.kind != RK_APP) return unsupported();
        const Desc* td = desc((uint64_t)I(0));
        if (!td || td->kind != RK_RTEMPLATE) return fail("not a render template");
        const auto& rs = renderersOf();
        if (td->a >= rs.size() || td->c < 0 || td->c >= (int64_t)rs[(size_t)td->a].templates.size()) return fail("the render template no longer exists");
        if (!host_.render) return fail("rendering is not available here");
        if (s.empty()) return fail("a render needs a file name");
        ScriptRenderRequest r;
        r.renderer = (int)td->a;
        r.tmpl = (int)td->c;
        r.output = s;
        r.markers = I(1) != 0;
        r.useRotation = I(2) != 0;
        r.from = da.size() > 0 && std::isfinite(da[0]) ? std::max(0.0, da[0]) : 0.0;
        const double length = da.size() > 1 && std::isfinite(da[1]) ? da[1] : -1.0;
        r.to = length < 0 ? project.length() : r.from + length;
        if (!(r.to > r.from)) {
            *dout = 1;      // NothingToDo
            return RS_OK;
        }
        std::string why;
        int status = 6;
        try {
            status = host_.render(project, r, &why);
        } catch (const std::exception& x) {
            why = x.what();
            status = 6;
        }
        if (status == 6 && !why.empty() && host_.log) host_.log("Render failed: " + why);
        if (status == 6 && !why.empty()) output.push_back("Render failed: " + why);
        *dout = status;
        return RS_OK;
    }
    case RO_APPLY_PRESET: {
        if (d.kind != RK_FX) return unsupported();
        FxSlot* slot = fx(d);
        if (!slot) return RS_FAILED;
        if (slot->kind != "ofx") return fail("presets are only supported by Reno for OpenFX effects");
        const ScriptPreset* found = nullptr;
        if (!ia.empty()) {
            const Desc* pd = desc((uint64_t)ia[0]);
            if (!pd || pd->kind != RK_PRESET) return fail("not a preset");
            if (pd->a >= host_.plugins.size() || host_.plugins[(size_t)pd->a].id != slot->pluginId) return fail("the preset is of another effect");
            const auto& v = presetsOf((size_t)pd->a);
            if (pd->c < 0 || pd->c >= (int64_t)v.size()) return fail("the preset no longer exists");
            found = &v[(size_t)pd->c];
        } else {
            for (size_t k = 0; k < host_.plugins.size() && !found; ++k) {
                if (host_.plugins[k].type == ScriptPlugin::Audio || host_.plugins[k].id != slot->pluginId) continue;
                for (const ScriptPreset& pr : presetsOf(k))
                    if (pr.name == s) { found = &pr; break; }
            }
            if (!found) return fail("this effect has no preset called \"" + s + "\"");
        }
        if (!slot->state.isObject()) {
            slot->state = Json::object();
            slot->state["plugin"] = slot->pluginId;
        }
        if (!slot->state.get("params").isObject()) slot->state["params"] = Json::object();
        Json params = slot->state.get("params");
        for (const auto& m : found->state.get("params").members()) params[m.first] = m.second;
        slot->state["params"] = params;
        slot->state["preset"] = true;
        fxStatesChanged.insert(slot->id);
        touch();
        return RS_OK;
    }
    case RO_ROOT_BIN:
        if (d.kind != RK_APP) return unsupported();
        *out = handle({RK_BIN, 0, 0, 0, ""});
        return RS_OK;
    case RO_NEW_BIN: {
        if (d.kind != RK_BIN) return unsupported();
        if (!binExists(d.s)) return fail("the media bin no longer exists");
        if (s.empty() || s.find('/') != std::string::npos) return fail("a media bin's name cannot be empty or hold a slash");
        const std::string path = d.s.empty() ? s : d.s + "/" + s;
        if (!binExists(path)) {
            project.mediaBins.push_back(path);
            touch();
        }
        *out = handle({RK_BIN, 0, 0, 0, path});
        return RS_OK;
    }
    case RO_BIN_ADD_MEDIA: {
        if (d.kind != RK_BIN) return unsupported();
        if (!binExists(d.s)) return fail("the media bin no longer exists");
        const Desc* md = desc((uint64_t)I(0));
        if (!md || md->kind != RK_MEDIA) return fail("not a media item");
        MediaItem* m = project.findMedia(md->a);
        if (!m) return fail("the media is no longer in the project");
        m->bin = d.s;
        if (!d.s.empty() && std::find(project.mediaBins.begin(), project.mediaBins.end(), d.s) == project.mediaBins.end())
            project.mediaBins.push_back(d.s);
        touch();
        return RS_OK;
    }
    }
    return kNotMine;
}

}  // namespace reno
