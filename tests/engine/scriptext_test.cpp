// Script engine tests (ScriptSession, the C++ side of VEGAS's scripting): HDR and stereoscopic 3D project settings,
// track groups and media bins. (The .NET side runs only on Windows: tests/scripting.)
//
//   scriptext_test
#include "../../src/engine/scriptsession.h"
#include <cmath>
#include <cstdio>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const uint64_t kApp = 1;

static int64_t geti(ScriptSession& s, uint64_t h, int prop, int* rc = nullptr)
{
    int64_t v = -12345;
    const int r = s.getI(h, prop, &v);
    if (rc) *rc = r;
    return v;
}
static std::vector<uint64_t> lst(ScriptSession& s, uint64_t h, int which)
{
    std::vector<uint64_t> v;
    s.list(h, which, &v);
    return v;
}
static uint64_t call(ScriptSession& s, uint64_t h, int op, const std::vector<int64_t>& ia, const std::string& str, int* rc = nullptr)
{
    uint64_t out = 0;
    double d = 0;
    const int r = s.call(h, op, ia, std::vector<double>(), str, &out, &d);
    if (rc) *rc = r;
    return out;
}
static std::string gets(ScriptSession& s, uint64_t h, int prop)
{
    std::string v;
    s.getS(h, prop, &v);
    return v;
}

int main()
{
    Project p;
    for (int k = 0; k < 3; ++k) {
        Track t;
        t.id = p.newId();
        t.type = k == 0 ? TrackType::Video : TrackType::Audio;
        p.tracks.push_back(t);
    }
    for (int k = 0; k < 3; ++k) {
        MediaItem m;
        m.id = p.newId();
        m.name = "clip" + std::to_string(k);
        m.path = "/x/clip" + std::to_string(k) + ".wav";
        p.media.push_back(m);
    }
    ScriptHost host;
    ScriptPlugin glow;
    glow.type = ScriptPlugin::VideoFilter;
    glow.id = "com.test:glow";
    glow.name = "Glow";
    host.plugins.push_back(glow);
    ScriptPlugin other = glow;
    other.id = "com.test:other";
    other.name = "Other";
    host.plugins.push_back(other);
    int presetReads = 0;
    host.presets = [&](const std::string& id, const std::string& context) {
        ++presetReads;
        std::vector<ScriptPreset> v;
        if (id != "com.test:glow" || context != "Filter") return v;
        ScriptPreset a, b;
        a.name = "Soft";
        a.state = Json::object();
        a.state["params"] = Json::object();
        Json amount = Json::object(), av = Json::array();
        av.push(0.25);
        amount["v"] = av;
        a.state["params"]["Amount"] = amount;
        b.name = "Hard";
        b.state = Json::object();
        b.state["params"] = Json::object();
        Json amount2 = Json::object(), bv = Json::array(), rv = Json::array();
        bv.push(0.95);
        amount2["v"] = bv;
        b.state["params"]["Amount"] = amount2;
        Json radius = Json::object();
        rv.push(40);
        radius["v"] = rv;
        b.state["params"]["Radius"] = radius;
        v.push_back(a);
        v.push_back(b);
        return v;
    };
    host.renderers = [] {
        std::vector<ScriptRenderer> v(2);
        v[0].name = "MainConcept AVC/AAC (*.mp4)";
        v[0].ext = "mp4";
        v[0].description = "H.264 in MP4";
        ScriptRenderTemplate a, b;
        a.name = "Internet 1280x720";
        a.width = 1280; a.height = 720; a.fpsNum = 30000; a.fpsDen = 1001; a.sampleRate = 48000; a.videoKbps = 5000; a.audioKbps = 192;
        b.name = "Audio only";
        b.video = false;
        b.ext = "m4a";
        v[0].templates = {a, b};
        v[1].name = "Wave (*.wav)";
        v[1].ext = "wav";
        v[1].hasVideo = false;
        ScriptRenderTemplate c;
        c.name = "44,100 Hz, 16 Bit, Stereo, PCM";
        c.video = false;
        c.sampleRate = 44100;
        v[1].templates = {c};
        return v;
    };
    ScriptRenderRequest lastRender;
    int renders = 0;
    host.render = [&](const Project&, const ScriptRenderRequest& r, std::string* error) {
        ++renders;
        lastRender = r;
        if (r.output == "/fail.mp4") { *error = "disk full"; return 6; }
        return 4;
    };
    ScriptSession s(p, host);
    const uint64_t app = s.kindOf(kApp) == RK_APP ? kApp : 0;
    check(app == kApp, "the app is handle 1");

    // ---------------------------------------------------------------- HDR and stereoscopic 3D
    check(geti(s, app, RP_HDR_MODE) == 2, "HDR mode starts Off (2)");
    check(s.setI(app, RP_HDR_MODE, 0) == RS_OK && s.project.settings.hdrMode == "hdr10" && geti(s, app, RP_HDR_MODE) == 0, "HDR10 is set");
    check(s.setI(app, RP_HDR_MODE, 1) == RS_OK && s.project.settings.hdrMode == "hlg" && geti(s, app, RP_HDR_MODE) == 1, "HLG is set");
    check(s.setI(app, RP_HDR_MODE, 7) == RS_FAILED, "a number that is no HDR mode fails");
    check(s.setI(app, RP_HDR_MODE, 2) == RS_OK && s.project.settings.hdrMode == "off", "Off is set");

    check(geti(s, app, RP_STEREO_MODE) == 0, "stereoscopic 3D starts Off");
    check(s.setI(app, RP_STEREO_MODE, 2) == RS_OK && s.project.settings.stereoMode == "sbsfull" && geti(s, app, RP_STEREO_MODE) == 2, "Side by side (full)");
    check(s.setI(app, RP_STEREO_MODE, 6) == RS_OK && s.project.settings.stereoMode == "anaglyph-ab", "Anaglyphic (amber/blue)");
    check(s.setI(app, RP_STEREO_MODE, 13) == RS_OK && s.project.settings.stereoMode == "difference" && geti(s, app, RP_STEREO_MODE) == 13, "Difference");
    check(s.setI(app, RP_STEREO_MODE, 14) == RS_FAILED && s.project.settings.stereoMode == "difference", "Left and Right is refused, the mode stays");
    check(s.setI(app, RP_STEREO_SWAP, 1) == RS_OK && s.project.settings.stereoSwap && geti(s, app, RP_STEREO_SWAP) == 1, "swap left and right");
    check(s.setD(app, RP_STEREO_CROSSTALK, 0.25) == RS_OK && s.project.settings.crosstalk == 0.25, "crosstalk cancelation");
    double ct = -1;
    check(s.getD(app, RP_STEREO_CROSSTALK, &ct) == RS_OK && ct == 0.25, "crosstalk reads back");
    check(s.setD(app, RP_STEREO_CROSSTALK, 1.5) == RS_FAILED, "crosstalk above 1 fails");
    check(s.setI(app, RP_STEREO_CROSSTALK_RENDER, 0) == RS_OK && !s.project.settings.crosstalkInRender, "crosstalk left out of renders");
    check(s.project.settings.stereo(), "the project then counts as stereoscopic");
    check(s.setI(app, RP_OUTPUT_360, 0) == RS_OK && s.project.settings.output360 && geti(s, app, RP_OUTPUT_360) == 0, "360 output On is 0");

    // ---------------------------------------------------------------- track groups
    const uint64_t t0 = s.trackHandle(p.tracks[0].id), t1 = s.trackHandle(p.tracks[1].id), t2 = s.trackHandle(p.tracks[2].id);
    check(lst(s, app, RL_TRACK_GROUPS).empty(), "no track groups at first");
    check(geti(s, t0, RP_TRACK_GROUP) == 0, "a track starts in no group");
    int rc = 0;
    call(s, app, RO_GROUP_SELECTED_TRACKS, {}, "", &rc);
    check(rc == RS_FAILED, "grouping with no tracks selected fails");
    s.selectedTracks.insert(p.tracks[1].id);
    s.selectedTracks.insert(p.tracks[2].id);
    const uint64_t g = call(s, app, RO_GROUP_SELECTED_TRACKS, {}, "", &rc);
    check(rc == RS_OK && g != 0 && s.kindOf(g) == RK_TRACKGROUP, "two selected tracks make a track group");
    check(lst(s, app, RL_TRACK_GROUPS).size() == 1 && lst(s, app, RL_TRACK_GROUPS)[0] == g, "it is listed");
    check(lst(s, g, RL_GROUP_TRACKS).size() == 2, "it holds both tracks");
    check(geti(s, t1, RP_TRACK_GROUP) == (int64_t)g && geti(s, t2, RP_TRACK_GROUP) == (int64_t)g && geti(s, t0, RP_TRACK_GROUP) == 0, "tracks know their group");
    check(geti(s, g, RP_IS_VALID) == 1, "the group is valid");
    check(geti(s, g, RP_MUTE) == 0, "not muted");
    check(s.setI(g, RP_MUTE, 1) == RS_OK && s.project.tracks[1].mute && s.project.tracks[2].mute && !s.project.tracks[0].mute, "muting the group mutes its tracks");
    check(geti(s, g, RP_MUTE) == 1, "and reads as muted");
    s.project.tracks[2].mute = false;
    check(geti(s, g, RP_MUTE) == 0, "one track unmuted: the group is not muted");
    check(s.setI(g, RP_SOLO, 1) == RS_OK && s.project.tracks[1].solo && s.project.tracks[2].solo, "soloing the group");
    call(s, g, RO_TRACKGROUP_ADD, {(int64_t)t0}, "", &rc);
    check(rc == RS_OK && lst(s, g, RL_GROUP_TRACKS).size() == 3, "a track is added to the group");
    call(s, g, RO_TRACKGROUP_ADD, {(int64_t)g}, "", &rc);
    check(rc == RS_FAILED, "something that is no track is refused");
    s.project.tracks[0].trackGroup = 0;
    s.project.tracks[1].trackGroup = 0;
    s.project.tracks[2].trackGroup = 0;
    check(geti(s, g, RP_IS_VALID) == 0, "an emptied group is no longer valid");

    // ---------------------------------------------------------------- media bins
    const uint64_t root = call(s, app, RO_ROOT_BIN, {}, "", &rc);
    check(rc == RS_OK && s.kindOf(root) == RK_BIN, "the root bin");
    check(gets(s, root, RP_NAME) == "Project Media", "the root is called Project Media");
    check(geti(s, root, RP_PARENT) == 0, "the root has no parent");
    check(lst(s, root, RL_BIN_BINS).empty() && lst(s, root, RL_BIN_MEDIA).size() == 3, "all media are in the root at first");
    const uint64_t day = call(s, root, RO_NEW_BIN, {}, "Interviews", &rc);
    check(rc == RS_OK && gets(s, day, RP_NAME) == "Interviews" && geti(s, day, RP_PARENT) == (int64_t)root, "a bin is made in the root");
    check(call(s, root, RO_NEW_BIN, {}, "Interviews", &rc) == day, "the same name gives the same bin");
    const uint64_t sub = call(s, day, RO_NEW_BIN, {}, "Day 1", &rc);
    check(rc == RS_OK && s.project.mediaBins.size() == 2 && s.project.mediaBins[1] == "Interviews/Day 1", "a bin inside a bin");
    check(geti(s, sub, RP_PARENT) == (int64_t)day, "its parent is the first bin");
    call(s, root, RO_NEW_BIN, {}, "a/b", &rc);
    check(rc == RS_FAILED, "a name with a slash is refused");
    const uint64_t m0 = s.mediaHandle(p.media[0].id), m1 = s.mediaHandle(p.media[1].id);
    call(s, sub, RO_BIN_ADD_MEDIA, {(int64_t)m0}, "", &rc);
    check(rc == RS_OK && s.project.media[0].bin == "Interviews/Day 1", "media are moved into a bin");
    check(lst(s, sub, RL_BIN_MEDIA).size() == 1 && lst(s, root, RL_BIN_MEDIA).size() == 2, "the lists follow");
    check(geti(s, m0, RP_MEDIA_BIN) == (int64_t)sub, "a media item knows its bin");
    check(geti(s, m1, RP_MEDIA_BIN) == (int64_t)root, "one with none is in the root");
    check(lst(s, day, RL_BIN_BINS).size() == 1 && lst(s, day, RL_BIN_BINS)[0] == sub, "a bin lists the bins in it");
    check(s.setS(day, RP_NAME, "Talks") == RS_OK && s.project.media[0].bin == "Talks/Day 1" && s.project.mediaBins[0] == "Talks", "renaming a bin renames the paths below it");
    check(gets(s, day, RP_NAME) == "Talks" && gets(s, sub, RP_NAME) == "Day 1" && geti(s, sub, RP_IS_VALID) == 1, "its handles follow the new name");
    check(s.setS(root, RP_NAME, "x") == RS_FAILED, "the root cannot be renamed");
    call(s, root, RO_NEW_BIN, {}, "Other", &rc);
    check(s.setS(day, RP_NAME, "Other") == RS_FAILED, "a name that is taken is refused");
    const uint64_t gone = call(s, root, RO_NEW_BIN, {}, "Temp", &rc);
    s.project.mediaBins.erase(std::find(s.project.mediaBins.begin(), s.project.mediaBins.end(), "Temp"));
    check(geti(s, gone, RP_IS_VALID) == 0, "a bin that was removed is no longer valid");
    check(s.changed, "the run counts as having changed the project");

    // ---------------------------------------------------------------- effect presets
    {
        std::vector<uint64_t> ps = lst(s, app, RL_VIDEO_FX);
        check(ps.size() == 2, "two video plug-ins");
        const uint64_t glowH = ps[0], otherH = ps[1];
        std::vector<uint64_t> gp = lst(s, glowH, RL_PRESETS);
        check(gp.size() == 2 && gets(s, gp[0], RP_NAME) == "Soft" && gets(s, gp[1], RP_NAME) == "Hard" && s.kindOf(gp[0]) == RK_PRESET, "a plug-in lists its presets in order");
        check(geti(s, gp[1], RP_INDEX) == 1, "a preset knows its place");
        check(lst(s, otherH, RL_PRESETS).empty(), "another plug-in has none");
        lst(s, glowH, RL_PRESETS);
        check(presetReads == 2, "presets are read once per plug-in", std::to_string(presetReads));
        const uint64_t track0 = s.trackHandle(p.tracks[0].id);
        const uint64_t fxH = call(s, track0, RO_ADD_FX, {(int64_t)glowH, -1}, "", &rc);
        check(rc == RS_OK && s.kindOf(fxH) == RK_FX, "an effect is added");
        std::vector<uint64_t> fp = lst(s, fxH, RL_PRESETS);
        check(fp.size() == 2 && fp[0] == gp[0], "the effect lists its plug-in's presets");
        call(s, fxH, RO_APPLY_PRESET, {(int64_t)gp[1]}, "", &rc);
        const FxSlot& slot = s.project.tracks[0].fx[0];
        check(rc == RS_OK && slot.state.get("params").get("Amount").get("v").at(0).asDouble() == 0.95 &&
                  slot.state.get("params").get("Radius").get("v").at(0).asDouble() == 40, "a preset's settings go into the effect");
        check(s.fxStatesChanged.count(slot.id) == 1, "the effect is marked as changed");
        call(s, fxH, RO_APPLY_PRESET, {}, "Soft", &rc);
        check(rc == RS_OK && slot.state.get("params").get("Amount").get("v").at(0).asDouble() == 0.25 &&
                  slot.state.get("params").get("Radius").get("v").at(0).asDouble() == 40, "a preset by name sets only what it holds");
        call(s, fxH, RO_APPLY_PRESET, {}, "Nothing", &rc);
        check(rc == RS_FAILED, "an unknown preset name fails");
        const uint64_t fx2 = call(s, track0, RO_ADD_FX, {(int64_t)otherH, -1}, "", &rc);
        call(s, fx2, RO_APPLY_PRESET, {(int64_t)gp[0]}, "", &rc);
        check(rc == RS_FAILED, "a preset of another plug-in is refused");
    }

    // ---------------------------------------------------------------- rendering
    {
        std::vector<uint64_t> rs = lst(s, app, RL_RENDERERS);
        check(rs.size() == 2 && s.kindOf(rs[0]) == RK_RENDERER && gets(s, rs[0], RP_NAME) == "MainConcept AVC/AAC (*.mp4)", "the formats are listed");
        check(gets(s, rs[0], RP_FILE_EXT) == "mp4" && gets(s, rs[0], RP_DESCRIPTION) == "H.264 in MP4" && geti(s, rs[1], RP_RT_HAS_VIDEO) == 0, "a format's extension, blurb and kind");
        std::vector<uint64_t> ts = lst(s, rs[0], RL_RENDER_TEMPLATES);
        check(ts.size() == 2 && s.kindOf(ts[0]) == RK_RTEMPLATE && gets(s, ts[0], RP_NAME) == "Internet 1280x720", "its templates");
        double fps = 0;
        check(geti(s, ts[0], RP_RT_WIDTH) == 1280 && geti(s, ts[0], RP_RT_HEIGHT) == 720 && s.getD(ts[0], RP_RT_FPS, &fps) == RS_OK && std::fabs(fps - 29.97) < 0.001 &&
                  geti(s, ts[0], RP_RT_VIDEO_KBPS) == 5000 && geti(s, ts[0], RP_RT_SAMPLE_RATE) == 48000, "a template's numbers");
        check(geti(s, ts[0], RP_RT_RENDERER) == (int64_t)rs[0], "a template knows its format");
        check(gets(s, ts[1], RP_FILE_EXT) == "m4a" && gets(s, ts[0], RP_FILE_EXT) == "mp4", "a template's own extension");
        uint64_t o = 0;
        double st = 0;
        s.call(app, RO_RENDER, {(int64_t)ts[0], 1, 0}, {2.0, 3.5}, "/out.mp4", &o, &st);
        check(st == 4 && renders == 1 && lastRender.renderer == 0 && lastRender.tmpl == 0 && lastRender.output == "/out.mp4" && lastRender.from == 2.0 && lastRender.to == 5.5 &&
                  lastRender.markers && !lastRender.useRotation, "a render passes its range and options on and completes");
        s.call(app, RO_RENDER, {(int64_t)ts[0], 0, 0}, {0.0, 0.0}, "/out.mp4", &o, &st);
        check(st == 1 && renders == 1, "a render of nothing is NothingToDo and not started");
        s.call(app, RO_RENDER, {(int64_t)ts[0], 0, 0}, {1.0, 1.0}, "/fail.mp4", &o, &st);
        check(st == 6 && !s.output.empty(), "a failure is Failed and logged");
        rc = s.call(app, RO_RENDER, {(int64_t)app, 0, 0}, {0.0, 1.0}, "/x.mp4", &o, &st);
        check(rc == RS_FAILED, "something that is no template is refused");
        rc = s.call(app, RO_RENDER, {(int64_t)ts[0], 0, 0}, {0.0, 1.0}, "", &o, &st);
        check(rc == RS_FAILED, "no file name is refused");
    }

    // ---------------------------------------------------------------- 3D track motion
    {
        double dt = 0;
        const uint64_t key = call(s, t0, RO_ADD_MOTION_KEY, {}, "", &rc);
        check(rc == RS_OK && s.kindOf(key) == RK_MOTION, "a track motion key");
        check(s.getD(key, RP_KEY_Z, &dt) == RS_OK && dt == 0 && s.getD(key, RP_KEY_DEPTH, &dt) == RS_OK && dt == 1.0, "its 3D numbers start at their defaults");
        const int props[] = {RP_KEY_Z, RP_KEY_ROT_X, RP_KEY_ROT_Y, RP_KEY_OFF_X, RP_KEY_OFF_Y, RP_KEY_OFF_Z, RP_KEY_ORI_X, RP_KEY_ORI_Y, RP_KEY_ORI_Z};
        bool all = true;
        for (int k = 0; k < 9; ++k) all = all && s.setD(key, props[k], 0.1 * (k + 1)) == RS_OK;
        const MotionKey& mk = s.project.tracks[0].motion[0];
        check(all && mk.z == 0.1 && mk.rotX == 0.2 && mk.rotY == 0.30000000000000004 && mk.offX == 0.4 && mk.offY == 0.5 && mk.offZ == 0.6000000000000001 &&
                  mk.oriX == 0.7000000000000001 && mk.oriY == 0.8 && mk.oriZ == 0.9, "each 3D number goes to the key");
        check(mk.has3d(), "the key then counts as 3D");
        check(s.setD(key, RP_KEY_DEPTH, 2.5) == RS_OK && mk.d == 2.5 && s.getD(key, RP_KEY_DEPTH, &dt) == RS_OK && dt == 2.5, "depth");
        check(s.setD(key, RP_KEY_DEPTH, -1) == RS_FAILED && mk.d == 2.5, "negative depth fails");
        check(s.getD(key, RP_KEY_ROT_Y, &dt) == RS_OK && dt == 0.30000000000000004, "reads back");
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
