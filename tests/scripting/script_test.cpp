// Tests running VEGAS scripts (Tools > Scripting > Run Script) on Linux: the .NET runtime (CoreCLR, from a PowerShell 7
// folder) is loaded into this process as Reno loads the .NET Framework on Windows, Reno's ScriptPortal.Vegas (compiled
// by run_tests.sh) is called through the same entry point (RenoHost.Run "api=<address>"), and real scripts (compiled
// beforehand: there is no CodeDOM compiler on Linux) edit a ScriptSession's project through the C interface. Then the
// project is checked here.
//   script_test <pwsh folder> <build folder with ScriptPortal.Vegas.dll and the compiled scripts> [sample projects folder]
#include "../../src/engine/scriptsession.h"
#include "../../src/vegas/vegas.h"
#include "../../src/core/json.h"
#include <dlfcn.h>
#include <dirent.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <functional>
#include <algorithm>

using namespace reno;

static int failures = 0, passes = 0;
#define CHECK(cond, ...) do { if (cond) ++passes; else { ++failures; std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ------------------------------------------------------------------------------------------------ the .NET runtime
typedef int (*coreclr_initialize_fn)(const char*, const char*, int, const char**, const char**, void**, unsigned*);
typedef int (*coreclr_create_delegate_fn)(void*, unsigned, const char*, const char*, const char*, void**);
typedef int (*RunFn)(const char* arg);
static RunFn runEntry = nullptr;

static bool startClr(const std::string& pwshDir, const std::string& buildDir)
{
    void* lib = dlopen((pwshDir + "/libcoreclr.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("cannot load libcoreclr.so: %s\n", dlerror()); return false; }
    auto init = (coreclr_initialize_fn)dlsym(lib, "coreclr_initialize");
    auto create = (coreclr_create_delegate_fn)dlsym(lib, "coreclr_create_delegate");
    if (!init || !create) return false;
    // every assembly of the runtime, and ours (the shim and the Windows Forms stand-in)
    std::string tpa;
    if (DIR* d = opendir(pwshDir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".dll" && n != "ScriptPortal.Vegas.dll" && n != "System.Windows.Forms.dll")
                tpa += pwshDir + "/" + n + ":";
        }
        closedir(d);
    }
    tpa += buildDir + "/ScriptPortal.Vegas.dll:" + buildDir + "/System.Windows.Forms.dll";
    const char* keys[] = {"TRUSTED_PLATFORM_ASSEMBLIES", "APP_PATHS", "NATIVE_DLL_SEARCH_DIRECTORIES"};
    const char* values[] = {tpa.c_str(), buildDir.c_str(), pwshDir.c_str()};
    void* host = nullptr;
    unsigned domain = 0;
    int r = init((buildDir + "/script_test").c_str(), "reno", 3, keys, values, &host, &domain);
    if (r < 0) { std::printf("coreclr_initialize failed: %x\n", r); return false; }
    r = create(host, domain, "ScriptPortal.Vegas", "ScriptPortal.Vegas.RenoHost", "Run", (void**)&runEntry);
    if (r < 0) { std::printf("coreclr_create_delegate failed: %x\n", r); return false; }
    return true;
}

// ------------------------------------------------------------------------------------------------ a project to work on
static ScriptHost makeHost(const std::string& script, const std::string& args = "")
{
    ScriptHost h;
    h.scriptFile = script;
    h.scriptArgs = args;
    h.installDir = "/opt/reno";
    // made-up media: *.mp4 picture and sound (10 s, 1280x720, 25 fps), *.wav sound, *.png a still
    h.probe = [](const std::string& path, MediaInfo* info, std::string* err) {
        auto ends = [&](const char* x) { return path.size() > strlen(x) && path.substr(path.size() - strlen(x)) == x; };
        MediaInfo m;
        m.path = path;
        if (ends(".mp4")) {
            m.hasVideo = m.hasAudio = true;
            m.width = 1280; m.height = 720; m.fps = 25; m.duration = 10; m.sampleRate = 48000; m.channels = 2;
            m.videoCodec = "h264"; m.audioCodec = "aac";
        } else if (ends(".wav") || ends(".mp3")) {
            m.hasAudio = true; m.duration = 300; m.sampleRate = 44100; m.channels = 2;
        } else if (ends(".png") || ends(".jpg")) {
            m.hasVideo = true; m.isStill = true; m.width = 800; m.height = 600;
        } else {
            if (err) *err = "unknown file";
            return false;
        }
        *info = m;
        return true;
    };
    auto plug = [](ScriptPlugin::Type t, const char* id, const char* name) {
        ScriptPlugin p;
        p.type = t;
        p.id = id;
        p.name = name;
        p.group = "Test";
        return p;
    };
    h.plugins.push_back(plug(ScriptPlugin::VideoFilter, "com.vegascreativesoftware:gaussianblur", "VEGAS Gaussian Blur"));
    h.plugins.push_back(plug(ScriptPlugin::VideoFilter, "com.test:glow", "Glow"));
    h.plugins.push_back(plug(ScriptPlugin::Transition, "com.vegascreativesoftware:crossfade", "VEGAS Cross Fade"));
    h.plugins.push_back(plug(ScriptPlugin::Generator, "com.vegascreativesoftware:solidcolor", "VEGAS Solid Color"));
    ScriptPlugin rev = plug(ScriptPlugin::Audio, "", "Reverb");
    rev.audio.format = "VST2";
    rev.audio.path = "/plugins/reverb.so";
    rev.audio.uid = "1234";
    rev.audio.name = "Reverb";
    rev.id = rev.audio.key();
    h.plugins.push_back(rev);
    // the OpenFX parameters of "Glow" (and none for the others: they cannot be read, as a plug-in that fails to load)
    h.describeOfx = [](const std::string& id, const std::string& ctx, ScriptOfxDesc* d, std::string* err) {
        if (id != "com.test:glow") { *err = "not installed"; return false; }
        d->label = "Glow";
        d->group = "Test";
        d->path = "/plugins/test.ofx";
        auto P = [&](const char* name, int type, int dim, bool whole, std::vector<double> def, std::vector<double> mn, std::vector<double> mx) {
            ScriptOfxParam p;
            p.name = name; p.label = name; p.type = type; p.dim = dim; p.whole = whole;
            p.def = def; p.min = mn; p.max = mx; p.displayMin = mn; p.displayMax = mx;
            d->params.push_back(p);
            return &d->params.back();
        };
        ScriptOfxParam* a = P("Amount", 11, 1, false, {0.5}, {0}, {1});
        a->label = "Glow amount"; a->hint = "How much";
        P("Radius", 8, 1, true, {10}, {0}, {100});
        P("Enable", 4, 1, true, {0}, {0}, {1});
        P("Mode", 5, 1, true, {0}, {0}, {2})->choices = {"Soft", "Hard", "Wild"};
        P("Tint", 7, 4, false, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1, 1});
        P("Center", 12, 2, false, {0.5, 0.5}, {0, 0}, {1, 1});
        P("Caption", 14, 0, false, {}, {}, {})->defText = "hello";
        ScriptOfxParam* page = P("Page", 1, 0, false, {}, {}, {});
        page->canAnimate = false; page->persist = false;
        (void)ctx;
        return true;
    };
    return h;
}

// V1 (video): clip.mp4 0..4 s (0.5 s into the media), A1 (audio): the same, grouped; V2 empty; still.png in the
// media; a marker at 1 s and a region 2..3 s. Both events selected, the cursor at 1.5 s.
static Project makeProject(ScriptHost& h)
{
    Project p;
    p.settings.width = 1920;
    p.settings.height = 1080;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    MediaItem clip, still;
    clip.id = p.newId();
    clip.path = "/media/clip.mp4";
    clip.name = "clip.mp4";
    h.probe(clip.path, &clip.info, nullptr);
    still.id = p.newId();
    still.path = "/media/still.png";
    still.name = "still.png";
    h.probe(still.path, &still.info, nullptr);
    p.media = {clip, still};
    Track v1, a1, v2;
    v1.id = p.newId(); v1.type = TrackType::Video; v1.name = "V1";
    a1.id = p.newId(); a1.type = TrackType::Audio; a1.name = "A1";
    v2.id = p.newId(); v2.type = TrackType::Video; v2.name = "V2";
    const uint64_t group = p.newId();
    Event e;
    e.mediaId = clip.id; e.start = 0; e.length = 4; e.offset = 0.5; e.name = "clip.mp4"; e.group = group;
    e.id = p.newId(); v1.events.push_back(e);
    e.id = p.newId(); a1.events.push_back(e);
    p.tracks = {v1, a1, v2};
    Project::Marker m;
    m.id = p.newId(); m.time = 1; m.name = "m1";
    p.markers.push_back(m);
    m.id = p.newId(); m.time = 2; m.length = 1; m.region = true; m.name = "r1";
    p.markers.push_back(m);
    return p;
}

static void select(ScriptSession& s)
{
    s.selection.clear();
    for (const Track& t : s.project.tracks)
        for (const Event& e : t.events) s.selection.insert(e.id);
    s.cursor = 1.5;
}

static bool run(ScriptSession& s, const char* what)
{
    char arg[64];
    std::snprintf(arg, sizeof arg, "api=%llx", (unsigned long long)(uintptr_t)s.api());
    const int r = runEntry(arg);
    for (const auto& line : s.output) std::printf("  [%s] %s\n", what, line.c_str());
    if (!s.finished) std::printf("  [%s] the script's side did not report the end (returned %d)\n", what, r);
    if (s.failed) std::printf("  [%s] failed: %s\n", what, s.message.c_str());
    return r == 0 && s.finished && !s.failed;
}

static double eps = 1e-6;

// ------------------------------------------------------------------------------------------------ the tests
static void apiTest(const std::string& b)
{
    ScriptHost h = makeHost(b + "/scripts/api_test.dll");
    Project p = makeProject(h);
    ScriptSession s(p, h);
    select(s);
    const bool ok = run(s, "api_test");
    CHECK(ok, "api_test.cs ran");
    bool said = false;
    for (const auto& l : s.output) said = said || l.rfind("API TEST OK", 0) == 0;
    CHECK(said, "the script reached its end");
    CHECK(s.changed, "the project was changed");
    CHECK(s.undoLabel == "API Test", "the UndoBlock names the step: %s", s.undoLabel.c_str());
    if (!ok) return;
    // what the script did, seen from Reno
    const Project& q = s.project;
    CHECK(q.tracks.size() == 3, "%zu tracks", q.tracks.size());
    const Track& v1 = q.tracks[0];
    CHECK(v1.events.size() == 3, "V1 has the two halves and the moved copy: %zu", v1.events.size());
    if (v1.events.size() == 3) {
        const Event& a = v1.events[0];
        const Event& r = v1.events[1];
        CHECK(std::fabs(a.length - 1) < eps && std::fabs(r.start - 1) < eps && std::fabs(r.offset - 1.5) < eps, "split");
        CHECK(r.rate == 2 && r.mute && !r.loop, "rate, mute, loop");
        CHECK(std::fabs(r.fadeIn - 10.0 / 30) < eps && r.fadeInCurve == CurveSlow && std::fabs(r.fadeOut - 5.0 / 30) < eps, "fades");
        CHECK(std::fabs(r.gain - 0.5) < eps, "level %g", r.gain);
        CHECK(r.transition.pluginId == "com.vegascreativesoftware:crossfade", "transition %s", r.transition.pluginId.c_str());
        CHECK(a.fx.empty(), "the effect was removed");
        CHECK(a.envelopes.size() == 1 && a.envelopes[0].kind == "velocity" && a.envelopes[0].points.size() == 2, "velocity envelope");
        if (!a.envelopes.empty() && a.envelopes[0].points.size() == 2) {
            CHECK(a.envelopes[0].points[0].value == -1 && a.envelopes[0].points[1].value == 2 && a.envelopes[0].points[1].curve == CurveSmooth, "points");
        }
        CHECK(a.panCrop.size() == 2, "pan/crop keys %zu", a.panCrop.size());
        if (a.panCrop.size() == 2) {
            const PanCropKey& k0 = a.panCrop[0];
            CHECK(std::fabs(k0.cx - 0.25) < 1e-4 && std::fabs(k0.cy - 0.25) < 1e-4 && std::fabs(k0.w - 0.5) < 1e-4 && std::fabs(k0.h - 0.5) < 1e-4,
                  "pan/crop box %g %g %g %g", k0.cx, k0.cy, k0.w, k0.h);
            const PanCropKey& k1 = a.panCrop[1];
            CHECK(std::fabs(k1.time - 0.5) < eps && std::fabs(k1.cx - 900.0 / 1280) < 1e-4 && std::fabs(k1.rotation + 90) < 1e-4,
                  "second key %g %g %g", k1.time, k1.cx, k1.rotation);
        }
        CHECK(!s.selection.count(a.id) && s.selection.count(r.id), "selection follows the split and the deselect");
        CHECK(v1.fx.size() == 1 && v1.fx[0].pluginId == "com.test:glow", "track effect");
        CHECK(v1.mute && v1.composite == "add", "track switches");
    }
    const Track& a1 = q.tracks[1];
    CHECK(a1.events.size() == 2, "A1 events %zu", a1.events.size());
    if (!a1.events.empty()) {
        const Event& b = a1.events[0];
        CHECK(b.pitch == 3 && b.keepPitch && b.stretchMethod == "classic" && b.stretchMode == "A05", "stretch %s %s", b.stretchMethod.c_str(), b.stretchMode.c_str());
        CHECK(b.channels == "combine" && b.invertPhase, "channels %s", b.channels.c_str());
        CHECK(b.fx.size() == 1 && b.fx[0].kind == "audio" && b.fx[0].audio.name == "Reverb", "audio effect");
    }
    CHECK(std::fabs(a1.level - 0.5) < 1e-6, "volume");
    CHECK(a1.envelopes.size() == 1 && a1.envelopes[0].kind == "volume" && a1.envelopes[0].points.size() == 2, "volume envelope");
    CHECK(q.media.size() == 3 && q.media[2].path == "/media/other.mp4", "media");
    int markers = 0, regions = 0;
    for (const auto& m : q.markers) (m.region ? regions : markers)++;
    CHECK(markers == 1 && regions == 2, "markers %d regions %d", markers, regions);
    CHECK(q.settings.title == "Script title", "summary");
    CHECK(std::fabs(s.cursor - 3) < eps && std::fabs(s.selStart - 1) < eps && std::fabs(s.selLength - 2) < eps, "cursor and selection");
    // the new event's group: the audio event made at 20 s is in it
    uint64_t g = 0;
    for (const Event& e : a1.events) if (std::fabs(e.start - 20) < eps) g = e.group;
    CHECK(g != 0, "new group");
}

// OpenFX parameters (VEGAS's OFXEffect): ofx_test.cs sets them; here the effect's settings in the project are checked.
static void ofxTest(const std::string& b)
{
    ScriptHost h = makeHost(b + "/scripts/ofx_test.dll");
    Project p = makeProject(h);
    ScriptSession s(p, h);
    const bool ok = run(s, "ofx_test");
    CHECK(ok, "ofx_test.cs ran");
    if (!ok) return;
    const Event& e = s.project.tracks[0].events[0];
    CHECK(e.fx.size() == 1, "the effect is on the event: %zu", e.fx.size());
    if (e.fx.empty()) return;
    const FxSlot& fx = e.fx[0];
    CHECK(s.fxStatesChanged.count(fx.id) == 1, "its settings are marked as changed");
    const Json& ps = fx.state.get("params");
    CHECK(fx.state.get("plugin").asString() == "com.test:glow", "plugin %s", fx.state.dump().c_str());
    const Json& amount = ps.get("Amount");
    CHECK(std::fabs(amount.get("v").at(0).asDouble() - 0.8) < 1e-9, "Amount %s", amount.dump().c_str());
    CHECK(amount.get("k").size() == 2 && std::fabs(amount.get("k").at(1).at(0).asDouble() - 60) < 1e-9 &&
              std::fabs(amount.get("k").at(1).at(1).asDouble() - 0.75) < 1e-9,
          "Amount's keys (frames, values) %s", amount.dump().c_str());
    CHECK(!amount.has("kc"), "linear keys keep no curves: %s", amount.dump().c_str());
    CHECK(ps.get("Radius").get("v").at(0).asDouble() == 42 && ps.get("Radius").get("i").asBool(), "Radius %s", ps.get("Radius").dump().c_str());
    CHECK(ps.get("Enable").get("v").at(0).asDouble() == 1, "Enable");
    CHECK(ps.get("Mode").get("v").at(0).asDouble() == 1 && ps.get("Mode").get("k").size() == 0, "Mode %s", ps.get("Mode").dump().c_str());
    CHECK(ps.get("Tint").get("v").size() == 4 && ps.get("Tint").get("v").at(3).asDouble() == 0.25, "Tint");
    CHECK(std::fabs(ps.get("Center").get("v").at(1).asDouble() - 0.9) < 1e-9, "Center");
    CHECK(ps.get("Caption").get("s").asString() == "Title", "Caption %s", ps.get("Caption").dump().c_str());
    CHECK(!ps.has("Page"), "a page has no value");
}

// HDR and stereoscopic 3D settings, track groups and media bins (groupsbins_test.cs).
static void groupsBinsTest(const std::string& b)
{
    ScriptHost h = makeHost(b + "/scripts/groupsbins_test.dll");
    Project p = makeProject(h);
    ScriptSession s(p, h);
    const bool ok = run(s, "groupsbins_test");
    CHECK(ok, "groupsbins_test.cs ran");
    if (!ok) return;
    const ProjectSettings& st = s.project.settings;
    CHECK(st.hdrMode == "hlg" && st.stereoMode == "sbsfull" && st.stereoSwap && std::fabs(st.crosstalk - 0.5) < 1e-6, "the project's HDR and 3D settings");
    CHECK(s.project.tracks[0].trackGroup != 0 && s.project.tracks[0].trackGroup == s.project.tracks[2].trackGroup &&
              s.project.tracks[1].trackGroup == s.project.tracks[0].trackGroup && s.project.tracks[0].mute, "the three tracks are in one muted group");
    CHECK(s.project.mediaBins.size() == 2 && s.project.mediaBins[0] == "Raw" && s.project.mediaBins[1] == "Raw/Day 1", "the bins");
    CHECK(s.project.media[0].bin == "Raw/Day 1", "the clip is in the inner bin");
}

// Every member a script can reach (members_test.cs): reading works or is a named NotSupportedException.
static void membersTest(const std::string& b)
{
    ScriptHost h = makeHost(b + "/scripts/members_test.dll");
    Project p = makeProject(h);
    ScriptSession s(p, h);
    select(s);
    const bool ok = run(s, "members_test");
    bool said = false;
    for (const auto& l : s.output) said = said || l == "MEMBERS OK";
    CHECK(ok && said, "every member read works or is named as not supported");
}

// A script that fails: the error comes back to Reno, and an unsupported member is named.
static void failureTests(const std::string& b)
{
    {
        ScriptHost h = makeHost(b + "/scripts/fails.dll");
        Project p = makeProject(h);
        ScriptSession s(p, h);
        CHECK(!run(s, "fails"), "the failing script fails");
        CHECK(s.finished && s.failed && s.message.find("Effect.Preset is not supported by Reno") != std::string::npos, "message: %s", s.message.c_str());
    }
    {
        ScriptHost h = makeHost(b + "/scripts/no_such_file.dll");
        Project p = makeProject(h);
        ScriptSession s(p, h);
        CHECK(!run(s, "missing"), "a missing script fails");
        CHECK(s.failed && s.message.find("not found") != std::string::npos, "message: %s", s.message.c_str());
    }
    {
        ScriptHost h = makeHost(b + "/scripts/noentry.dll");
        Project p = makeProject(h);
        ScriptSession s(p, h);
        CHECK(!run(s, "noentry"), "a script with no entry point fails");
        CHECK(s.failed && s.message.find("no entry point") != std::string::npos, "message: %s", s.message.c_str());
    }
}

// VEGAS's own sample script (Script Menu folder of VEGAS Pro 18): groups video events with the audio events of the
// same media at the same place.
static void vegasGroupTest(const std::string& b)
{
    ScriptHost h = makeHost(b + "/scripts/Group Video and Audio Events.dll");
    Project p = makeProject(h);
    for (Track& t : p.tracks) for (Event& e : t.events) e.group = 0;
    ScriptSession s(p, h);
    CHECK(run(s, "VEGAS: Group Video and Audio Events"), "VEGAS's Group Video and Audio Events ran");
    const Event& v = s.project.tracks[0].events[0];
    const Event& a = s.project.tracks[1].events[0];
    CHECK(v.group != 0 && v.group == a.group, "grouped");
}

// A user's own script that builds a whole project (RENO_USER_SCRIPTS): its media files are not here, so it stops
// early with warnings in its log, but everything it does until then must work or fail cleanly.
static void userBuildTest(const std::string& b, const std::string& dir)
{
    // (run_tests.sh puts the compiled script in userbuild/ with empty files of the names of its pictures and songs)
    ScriptHost h = makeHost(b + "/userbuild/Build video project.dll");
    std::vector<std::string> saved;
    h.save = [&](const Project& p, const std::string& path, std::string*) { saved.push_back(path); return true; };
    Project p = makeProject(h);
    ScriptSession s(p, h);
    const bool ok = run(s, "user: Build video project");
    CHECK(ok, "the build script ran to its end");
    CHECK(s.startedNew, "it started a new project");
    CHECK(saved.size() == 1, "it saved the project (%zu)", saved.size());
    CHECK(!s.project.tracks.empty(), "it made tracks: %zu", s.project.tracks.size());
}

// The "Export to Reno" script (tools/vegas, written for VEGAS) run on the sample .veg projects: it reads
// everything through the API and writes a .reno, which must agree with the project Reno read.
static void exportToRenoTest(const std::string& b, const std::string& vegDir)
{
    std::vector<std::string> vegs;
    std::function<void(const std::string&)> walk = [&](const std::string& dir) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            if (e->d_type == DT_DIR) walk(dir + "/" + n);
            else if (n.size() > 4 && n.substr(n.size() - 4) == ".veg") vegs.push_back(dir + "/" + n);
        }
        closedir(d);
    };
    walk(vegDir);
    if (vegs.empty()) { std::printf("(no .veg samples: Export to Reno not run)\n"); return; }
    std::sort(vegs.begin(), vegs.end());
    int ran = 0, same = 0;
    for (const std::string& path : vegs) {
        Project p;
        std::string err;
        if (!loadProjectFile(path, p, &err)) continue;
        ScriptHost h = makeHost(b + "/scripts/Export to Reno.dll");
        // (untitled, so the script asks where to write - never next to the samples, which are read-only)
        const std::string out = b + "/export.reno";     // (RENO_TEST_SAVE_PATH, set before .NET started: its dialogs answer with it)
        std::remove(out.c_str());
        ScriptSession s(p, h);
        const bool ok = run(s, ("Export to Reno: " + pathFileName(path)).c_str());
        CHECK(ok, "Export to Reno ran on %s", path.c_str());
        if (!ok) continue;
        ++ran;
        Project q;
        CHECK(q.load(out, &err), "%s: the .reno it wrote loads: %s", path.c_str(), err.c_str());
        // the same tracks, events, places and media as Reno's own reading
        bool match = q.tracks.size() == p.tracks.size();
        for (size_t t = 0; match && t < p.tracks.size(); ++t) {
            match = q.tracks[t].events.size() == p.tracks[t].events.size() && q.tracks[t].type == p.tracks[t].type;
            for (size_t k = 0; match && k < p.tracks[t].events.size(); ++k) {
                const Event& x = p.tracks[t].events[k];
                const Event& y = q.tracks[t].events[k];
                // (the script folds a velocity envelope into the rate - Reno keeps the envelope - so the rate is compared
                // only for events without one)
                const bool velocity = findEnvelope(x.envelopes, "velocity") != nullptr;
                match = std::fabs(x.start - y.start) < 1e-4 && std::fabs(x.length - y.length) < 1e-4 && (velocity || std::fabs(x.rate - y.rate) < 1e-6) &&
                        x.mute == y.mute;
                if (!match) std::printf("  %s: track %zu event %zu: %g+%g x%g vs %g+%g x%g\n", pathFileName(path).c_str(), t, k, x.start, x.length,
                                        x.rate, y.start, y.length, y.rate);
            }
        }
        CHECK(match, "%s: the exported project matches Reno's reading", path.c_str());
        if (match) ++same;
    }
    std::printf("Export to Reno: ran on %d projects, %d matched\n", ran, same);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: script_test <pwsh folder> <build folder> [samples] [user script folder]\n");
        return 2;
    }
    const std::string pwsh = argv[1], b = argv[2];
    setenv("RENO_TEST_SAVE_PATH", (b + "/export.reno").c_str(), 1);
    setenv("RENO_TEST_FOLDER", b.c_str(), 1);     // (.NET reads the environment once, when it starts)
    if (!startClr(pwsh, b)) return 1;
    apiTest(b);
    ofxTest(b);
    groupsBinsTest(b);
    membersTest(b);
    failureTests(b);
    vegasGroupTest(b);
    if (argc > 4) userBuildTest(b, argv[4]);
    if (argc > 3) exportToRenoTest(b, argv[3]);
    std::printf("SUMMARY: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
