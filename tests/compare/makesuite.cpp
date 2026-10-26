// makesuite: the VEGAS-vs-Reno comparison suite. Small projects that each try one thing (cuts, fades of every curve,
// cross-fades, transitions, pan/crop, track motion, compositing modes, parent/child compositing, event rates, reverse,
// velocity, audio volume/pan/envelopes/normalize, generated media, VEGAS's OpenFX effects, a nested project, frame
// rates, odd frame sizes, audio only, stills), made with Reno's own project model. For each:
//   suite/<id>.reno        the project as a Reno project file: Reno Bridge builds it in VEGAS (Import Reno
//                          Project.cs) and VEGAS saves it as suite/<id>.veg, a .veg VEGAS wrote itself
//   suite/<id>.reno.veg    the same project as Reno writes it (for checking Reno's .veg writer in VEGAS)
//   jobs/<n>_<id>.job.json the Reno Bridge job: build, save, open the saved .veg, describe it, export it, render it
//   jobs_renoveg/...       optional jobs: VEGAS opens and renders Reno's own .veg of each test
//   manifest.json          the list: what each test is, its length and frame size, the plug-ins it needs and
//                          whether Reno can run them on Linux (VEGAS's own OpenFX plug-ins are Windows programs;
//                          Reno has stand-ins for the common ones)
// The media (tests/compare/make_media.sh) must be in <bridge>/suite first: their sizes and lengths are read here.
// Media paths in the projects are the Windows paths the files will have on the VEGAS machine
// (<windows bridge folder>\suite\<file>); Reno finds them beside the project wherever it is.
//
//   makesuite <bridge folder> [--win-root C:\path\to\bridge folder (as VEGAS sees it; default C:\Reno Bridge)] [--only id,id...]
#include "../../src/engine/project.h"
#include "../../src/media/media.h"
#include "../../src/ofx/builtins.h"
#include "../../src/vegas/vegas.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

using namespace reno;
namespace fs = std::filesystem;

static std::string g_root, g_win = "C:\\Reno Bridge";
static std::set<std::string> g_builtins;

static bool writeText(const std::string& path, const std::string& text)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(text.data(), 1, text.size(), f);
    return std::fclose(f) == 0;
}

// ------------------------------------------------------------------------------------------ building projects
struct Test {
    std::string id, title, what;
    Project p;
    bool audioOnly = false;
    bool fontDependent = false;
    std::vector<std::string> needs;     // plug-in identifiers
    std::string note;
};

static Project newProject(int w = 320, int h = 180, int num = 25, int den = 1)
{
    Project p;
    p.settings.width = w;
    p.settings.height = h;
    p.settings.fpsNum = num;
    p.settings.fpsDen = den;
    p.settings.sampleRate = 48000;
    p.settings.renderQuality = "best";
    p.settings.motionBlur = "gaussian";
    p.quickFade = 0;
    return p;
}

// media file in suite/ (read for its size, rate and length); its path is the Windows one
static uint64_t media(Project& p, const std::string& file)
{
    for (const auto& m : p.media)
        if (m.name == file) return m.id;
    MediaItem m;
    m.id = p.newId();
    m.name = file;
    const std::string local = g_root + "/suite/" + file;
    std::string err;
    if (file.size() > 4 && file.substr(file.size() - 4) == ".veg") {
        // (VEGAS's .veg of a nested project may not be here yet: Reno's own of the same project says the same)
        std::string have = local;
        if (!fs::exists(have)) have = local.substr(0, local.size() - 4) + ".reno.veg";
        if (!vegasProjectMediaInfo(have, m.info, &err)) std::fprintf(stderr, "warning: %s: %s\n", file.c_str(), err.c_str());
    } else {
        auto r = openMedia(local, &err);
        if (r) m.info = r->info();
        else std::fprintf(stderr, "warning: %s: %s (run make_media.sh first)\n", local.c_str(), err.c_str());
    }
    m.path = g_win + "\\suite\\" + file;
    m.info.path = m.path;
    p.media.push_back(m);
    return m.id;
}

static Json params(std::initializer_list<std::pair<const char*, std::vector<double>>> ps)
{
    Json j = Json::object();
    Json q = Json::object();
    for (const auto& kv : ps) {
        Json e = Json::object();
        Json v = Json::array();
        for (double d : kv.second) v.push(d);
        e["v"] = v;
        q[kv.first] = e;
    }
    j["params"] = q;
    return j;
}

static FxSlot fxSlot(Project& p, Test& t, const std::string& name, const Json& state = Json::object())
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = "com.vegascreativesoftware:" + name;
    s.name = name;
    s.state = state.isObject() ? state : Json::object();
    t.needs.push_back(s.pluginId);
    return s;
}

static uint64_t generated(Project& p, Test& t, const std::string& plugin, const Json& state, const std::string& label)
{
    MediaItem m;
    m.id = p.newId();
    m.name = label;
    m.generated = true;
    m.generator = fxSlot(p, t, plugin, state);
    m.generator.name = label;
    m.info.hasVideo = true;
    m.info.width = p.settings.width;
    m.info.height = p.settings.height;
    m.info.fps = p.settings.fps();
    p.media.push_back(m);
    return m.id;
}

static Track& track(Project& p, TrackType type, const std::string& name = std::string())
{
    Track t;
    t.id = p.newId();
    t.type = type;
    t.name = name;
    p.tracks.push_back(t);
    return p.tracks.back();
}

static Event& event(Project& p, Track& t, uint64_t mediaId, double start, double length, double offset = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = mediaId;
    e.start = start;
    e.length = length;
    e.offset = offset;
    setDefaultFadeCurves(e, t.type);
    t.events.push_back(e);
    return t.events.back();
}

// a clip's picture and sound, grouped, as VEGAS adds a file with both
static std::pair<Event*, Event*> clip(Project& p, Track& v, Track& a, const std::string& file, double start, double length, double offset = 0)
{
    const uint64_t m = media(p, file);
    Event& ve = event(p, v, m, start, length, offset);
    Event& ae = event(p, a, m, start, length, offset);
    ve.group = ae.group = p.newId();
    return {&ve, &ae};
}

static Envelope envelope(const std::string& kind, std::initializer_list<std::array<double, 3>> pts)
{
    Envelope e;
    e.kind = kind;
    for (const auto& q : pts) e.points.push_back({q[0], q[1], (int)q[2]});
    return e;
}

// ------------------------------------------------------------------------------------------ the tests
static std::vector<Test> makeTests()
{
    std::vector<Test> all;
    auto add = [&](const std::string& id, const std::string& title, const std::function<void(Test&, Project&)>& build) {
        Test t;
        t.id = id;
        t.title = title;
        t.p = newProject();
        build(t, t.p);
        all.push_back(std::move(t));
    };
    // two tracks (video over audio) with one clip
    auto va = [](Project& p) -> std::pair<Track*, Track*> {
        track(p, TrackType::Video, "Video");
        track(p, TrackType::Audio, "Audio");
        return {&p.tracks[0], &p.tracks[1]};
    };

    // ---- cuts and trims
    add("cut_basic", "Two clips cut together at 1 s", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        clip(p, *v, *a, "clip_a.avi", 0, 1.0);
        clip(p, *v, *a, "clip_b.avi", 1.0, 1.0);
    });
    add("cut_offset", "An event starting 1.52 s into its media (take offset)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        clip(p, *v, *a, "clip_a.avi", 0, 2.0, 1.52);
    });
    add("cut_gap", "A gap between two events (black and silence)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        clip(p, *v, *a, "clip_a.avi", 0, 0.8);
        clip(p, *v, *a, "clip_b.avi", 1.2, 0.8, 0.5);
    });
    add("cut_subframe", "Events starting between frames (0.013 s, 1.031 s)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        clip(p, *v, *a, "clip_a.avi", 0.013, 1.018);
        clip(p, *v, *a, "clip_b.avi", 1.031, 0.969, 0.27);
    });

    // ---- fades of each curve (picture and sound together)
    const std::pair<const char*, int> curves[] = {{"linear", CurveLinear}, {"fast", CurveFast}, {"slow", CurveSlow}, {"smooth", CurveSmooth}, {"sharp", CurveSharp}};
    for (const auto& c : curves)
        add(std::string("fade_") + c.first, std::string("Fade in and out, ") + c.first + " curve, picture and sound", [&](Test&, Project& p) {
            auto [v, a] = va(p);
            auto ev = clip(p, *v, *a, "clip_a.avi", 0, 2.0);
            for (Event* e : {ev.first, ev.second}) {
                e->fadeIn = 0.8;
                e->fadeOut = 0.8;
                e->fadeInCurve = e->fadeOutCurve = c.second;
            }
        });

    // ---- cross-fades
    add("xfade_default", "Cross-fade of 0.6 s with VEGAS's default curves (smooth pictures, fast/slow sound)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        auto e1 = clip(p, *v, *a, "clip_a.avi", 0, 1.3);
        auto e2 = clip(p, *v, *a, "clip_b.avi", 0.7, 1.3);
        for (Event* e : {e1.first, e1.second}) e->fadeOut = 0.6;
        for (Event* e : {e2.first, e2.second}) e->fadeIn = 0.6;
    });
    add("xfade_linear", "Cross-fade of 0.6 s, linear curves both ways", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        auto e1 = clip(p, *v, *a, "clip_a.avi", 0, 1.3);
        auto e2 = clip(p, *v, *a, "clip_b.avi", 0.7, 1.3);
        for (Event* e : {e1.first, e1.second}) { e->fadeOut = 0.6; e->fadeOutCurve = CurveLinear; }
        for (Event* e : {e2.first, e2.second}) { e->fadeIn = 0.6; e->fadeInCurve = CurveLinear; e->fadeInRecipCurve = CurveLinear; }
    });
    add("xfade_sharp_slow", "Cross-fade of 0.6 s, sharp in over slow out", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        auto e1 = clip(p, *v, *a, "clip_a.avi", 0, 1.3);
        auto e2 = clip(p, *v, *a, "clip_b.avi", 0.7, 1.3);
        for (Event* e : {e1.first, e1.second}) { e->fadeOut = 0.6; e->fadeOutCurve = CurveSlow; }
        for (Event* e : {e2.first, e2.second}) { e->fadeIn = 0.6; e->fadeInCurve = CurveSharp; e->fadeInRecipCurve = CurveSlow; }
    });

    // ---- transitions (VEGAS's OpenFX transitions; Reno has none of its own)
    for (const char* tr : {"dissolve", "linearwipe", "clockwipe", "push", "iris"})
        add(std::string("transition_") + tr, std::string("Transition: ") + tr + " (defaults), 0.8 s", [&, tr](Test& t, Project& p) {
            auto [v, a] = va(p);
            auto e1 = clip(p, *v, *a, "clip_a.avi", 0, 1.4);
            auto e2 = clip(p, *v, *a, "clip_b.avi", 0.6, 1.4);
            e1.first->fadeOut = e1.second->fadeOut = 0.8;
            e2.first->fadeIn = e2.second->fadeIn = 0.8;
            e2.first->transition = fxSlot(p, t, tr);
        });

    // ---- event opacity, track opacity
    add("event_opacity", "Event opacity 50 % over another track", [&](Test&, Project& p) {
        track(p, TrackType::Video, "Top");
        track(p, TrackType::Video, "Bottom");
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 2).gain = 0.5;
        event(p, p.tracks[1], media(p, "clip_b.avi"), 0, 2);
    });
    add("track_opacity", "Track level 40 % over another track", [&](Test&, Project& p) {
        track(p, TrackType::Video, "Top").level = 0.4;
        track(p, TrackType::Video, "Bottom");
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 2);
        event(p, p.tracks[1], media(p, "clip_b.avi"), 0, 2);
    });
    add("track_opacity_env", "Track composite level envelope 0 -> 100 % (linear) and back (smooth)", [&](Test&, Project& p) {
        Track& top = track(p, TrackType::Video, "Top");
        top.envelopes.push_back(envelope("composite", {{0, 0, CurveLinear}, {1, 1, CurveSmooth}, {2, 0, CurveLinear}}));
        track(p, TrackType::Video, "Bottom");
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 2);
        event(p, p.tracks[1], media(p, "clip_b.avi"), 0, 2);
    });

    // ---- stills and the frame's shape
    add("still_letterbox", "A 400x300 still in a 320x180 frame (aspect kept: pillarbox)", [&](Test&, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "still.png"), 0, 1.0);
    });
    add("still_stretch", "A 400x300 still stretched to the frame (Maintain aspect ratio off)", [&](Test&, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "still.png"), 0, 1.0).keepAspect = false;
    });
    add("still_alpha", "A PNG with transparency over a clip", [&](Test&, Project& p) {
        track(p, TrackType::Video, "PNG");
        track(p, TrackType::Video, "Clip");
        event(p, p.tracks[0], media(p, "still_alpha.png"), 0, 1.5);
        event(p, p.tracks[1], media(p, "clip_a.avi"), 0, 1.5);
    });

    // ---- pan/crop
    auto pc = [&](const std::string& id, const std::string& title, std::vector<PanCropKey> keys, bool keepAspect = true) {
        add(id, title, [=](Test&, Project& p) {
            track(p, TrackType::Video);
            Event& e = event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.6);
            e.panCrop = keys;
            e.keepAspect = keepAspect;
        });
    };
    pc("pancrop_zoom", "Pan/crop: the middle half, zoomed in", {{0, 0.5, 0.5, 0.5, 0.5, 0, 0}});
    pc("pancrop_offset", "Pan/crop: a quarter-size box at the top left", {{0, 0.3, 0.3, 0.4, 0.4, 0, 0}});
    pc("pancrop_zoomout", "Pan/crop: zoomed out (box twice the picture)", {{0, 0.5, 0.5, 2.0, 2.0, 0, 0}});
    pc("pancrop_rotate", "Pan/crop: turned 30 degrees", {{0, 0.5, 0.5, 1, 1, 30, 0}});
    pc("pancrop_flip_h", "Pan/crop: flipped horizontally", {{0, 0.5, 0.5, -1, 1, 0, 0}});
    pc("pancrop_flip_v", "Pan/crop: flipped vertically", {{0, 0.5, 0.5, 1, -1, 0, 0}});
    pc("pancrop_keys_linear", "Pan/crop: keyframes, zoom 1 -> 0.4 (linear)", {{0, 0.5, 0.5, 1, 1, 0, 0}, {1.5, 0.6, 0.4, 0.4, 0.4, 0, 0}});
    pc("pancrop_keys_smooth", "Pan/crop: keyframes, zoom and turn (smooth)", {{0, 0.5, 0.5, 1, 1, 0, 2}, {1.5, 0.4, 0.6, 0.5, 0.5, 45, 2}});
    pc("pancrop_aspect_off", "Pan/crop: a tall box with Maintain aspect ratio off", {{0, 0.5, 0.5, 0.3, 0.9, 0, 0}}, false);

    // ---- track motion
    auto tm = [&](const std::string& id, const std::string& title, std::vector<MotionKey> keys) {
        add(id, title, [=](Test&, Project& p) {
            track(p, TrackType::Video, "Moved").motion = keys;
            track(p, TrackType::Video, "Under");
            event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.6);
            event(p, p.tracks[1], media(p, "clip_b.avi"), 0, 1.6);
        });
    };
    MotionKey k1; k1.x = 0.3; k1.y = 0.3; k1.w = 0.5; k1.h = 0.5;
    tm("motion_scale", "Track motion: half size at the top left", {k1});
    MotionKey k2; k2.rotation = 25; k2.w = 0.7; k2.h = 0.7;
    tm("motion_rotate", "Track motion: 70 % size, turned 25 degrees", {k2});
    MotionKey k3a; k3a.w = 0.4; k3a.h = 0.4; k3a.x = 0.25; k3a.y = 0.5;
    MotionKey k3b = k3a; k3b.time = 1.5; k3b.x = 0.75; k3b.rotation = 90;
    tm("motion_keys", "Track motion: moves right and turns over 1.5 s", {k3a, k3b});

    // ---- compositing modes
    for (const char* mode : {"add", "subtract", "multiply", "screen", "overlay", "hardlight", "dodge", "burn", "darken", "lighten", "difference", "diffsquared", "cut"})
        add(std::string("composite_") + mode, std::string("Compositing mode ") + mode + ": clip B over clip A", [&, mode](Test&, Project& p) {
            track(p, TrackType::Video, "Top").composite = mode;
            track(p, TrackType::Video, "Bottom");
            event(p, p.tracks[0], media(p, "clip_b.avi"), 0, 1.2);
            event(p, p.tracks[1], media(p, "clip_a.avi"), 0, 1.2);
        });
    add("parent_child", "Compositing parent (multiply) with a child track (moved, half size) over a clip", [&](Test&, Project& p) {
        Track& parent = track(p, TrackType::Video, "Parent");
        parent.parentComposite = "multiply";
        Track& child = track(p, TrackType::Video, "Child");
        child.compositeLevel = 1;
        MotionKey k; k.w = 0.5; k.h = 0.5; k.x = 0.6; k.y = 0.6;
        child.motion = {k};
        track(p, TrackType::Video, "Under");
        event(p, p.tracks[0], media(p, "still_alpha.png"), 0, 1.2);
        event(p, p.tracks[1], media(p, "clip_b.avi"), 0, 1.2);
        event(p, p.tracks[2], media(p, "clip_a.avi"), 0, 1.2);
        (void)parent;
    });

    // ---- time: rate, reverse, velocity, loop
    add("rate_double", "Event playback rate 2 (picture and sound)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        auto e = clip(p, *v, *a, "clip_a.avi", 0, 2.0);
        e.first->rate = e.second->rate = 2.0;
    });
    add("rate_half", "Event playback rate 0.5 (picture and sound)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        auto e = clip(p, *v, *a, "clip_a.avi", 0, 2.0, 1.0);
        e.first->rate = e.second->rate = 0.5;
    });
    add("reverse", "A reversed event (picture)", [&](Test&, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 2.0, 1.0).reverse = true;
    });
    add("velocity_env", "Velocity envelope 100 % -> 300 % -> -100 % (linear)", [&](Test&, Project& p) {
        track(p, TrackType::Video);
        Event& e = event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 2.0, 0.5);
        e.envelopes.push_back(envelope("velocity", {{0, 1, CurveLinear}, {1.0, 3, CurveLinear}, {1.5, -1, CurveLinear}}));
    });
    add("loop_past_end", "An event longer than its media (loops back to the start)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        clip(p, *v, *a, "clip_5994.avi", 0, 3.0, 1.0);
        p.tracks.pop_back();        // (clip_5994 has no sound)
    });
    add("no_loop_hold", "An event longer than its media with Loop off (holds the last frame)", [&](Test&, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_5994.avi"), 0, 3.0, 1.0).loop = false;
    });
    add("undersample", "Undersample rate 0.5 (new frames at half the project's rate)", [&](Test&, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.6).undersample = 0.5;
    });

    // ---- sound
    auto audio = [&](const std::string& id, const std::string& title, const std::function<void(Project&, Track&, Event&)>& f, const char* file = "tone.wav") {
        add(id, title, [=](Test& t, Project& p) {
            t.audioOnly = true;
            Track& a = track(p, TrackType::Audio, "Audio");
            Event& e = event(p, a, media(p, file), 0, 2.5);
            f(p, a, e);
        });
    };
    audio("audio_plain", "Audio only: a stereo tone as it is", [](Project&, Track&, Event&) {});
    audio("audio_track_volume", "Track volume -6 dB", [](Project&, Track& a, Event&) { a.level = 0.5; });
    audio("audio_event_gain", "Event gain -12 dB", [](Project&, Track&, Event& e) { e.gain = 0.25; });
    audio("audio_pan_left", "Track pan 70 % left (VEGAS's default pan type)", [](Project&, Track& a, Event&) { a.pan = -0.7; });
    audio("audio_pan_right", "Track pan 40 % right", [](Project&, Track& a, Event&) { a.pan = 0.4; });
    audio("audio_volume_env", "Track volume envelope: -inf -> 0 dB (fast), down to -12 dB (smooth)", [](Project&, Track& a, Event&) {
        a.envelopes.push_back(envelope("volume", {{0, 0, CurveFast}, {1.0, 1, CurveSmooth}, {2.0, 0.25, CurveLinear}}));
    });
    audio("audio_pan_env", "Track pan envelope: left -> right (linear)", [](Project&, Track& a, Event&) {
        a.envelopes.push_back(envelope("pan", {{0, -1, CurveLinear}, {2.0, 1, CurveLinear}}));
    });
    audio("audio_normalize", "Normalize on a quiet (-30 dBFS) tone", [](Project&, Track&, Event& e) { e.normalize = true; e.normalizeGain = 31.0; }, "quiet.wav");
    audio("audio_invert_event", "Invert phase of the event (alone it sounds the same; the samples are negated)", [](Project&, Track&, Event& e) { e.invertPhase = true; });
    audio("audio_invert_cancel", "The same tone twice, one track inverted: silence", [](Project& p, Track&, Event&) {
        Track& b = track(p, TrackType::Audio, "Inverted");
        b.invertPhase = true;
        event(p, p.tracks[1], media(p, "tone.wav"), 0, 2.5);
    });
    audio("audio_rate", "Audio event at rate 1.5 (no time stretch: speed and pitch change)", [](Project&, Track&, Event& e) {
        e.rate = 1.5;
        e.stretchMethod = "none";
    });
    audio("audio_44k", "A 44.1 kHz file in a 48 kHz project", [](Project&, Track&, Event&) {}, "tone44.wav");
    audio("audio_clicks_mix", "Clicks over a tone on two tracks (timing to the sample)", [](Project& p, Track&, Event&) {
        track(p, TrackType::Audio, "Clicks");
        event(p, p.tracks[1], media(p, "clicks.wav"), 0.0101, 2.0, 0.05);
    });
    audio("audio_fades_xfade", "Audio cross-fade (fast / slow) of two files", [](Project& p, Track& a, Event& e) {
        e.length = 1.5;
        e.fadeOut = 0.5;
        Event& f = event(p, a, media(p, "tone44.wav"), 1.0, 1.5);
        f.fadeIn = 0.5;
    });

    // ---- generated media
    add("gen_solidcolor", "Generated media: Solid Color (orange)", [&](Test& t, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], generated(p, t, "solidcolor", params({{"Color", {1.0, 0.5, 0.1, 1.0}}}), "Solid Color"), 0, 1);
    });
    add("gen_testpattern", "Generated media: Test Pattern (its default)", [&](Test& t, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], generated(p, t, "testpattern", Json::object(), "Test Pattern"), 0, 1);
    });
    add("gen_checkerboard", "Generated media: Checkerboard (defaults)", [&](Test& t, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], generated(p, t, "checkerboard", Json::object(), "Checkerboard"), 0, 1);
    });
    add("gen_gradient", "Generated media: Color Gradient (defaults)", [&](Test& t, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], generated(p, t, "colorgradient", Json::object(), "Color Gradient"), 0, 1);
    });
    add("gen_noise", "Generated media: Noise Texture (defaults)", [&](Test& t, Project& p) {
        track(p, TrackType::Video);
        event(p, p.tracks[0], generated(p, t, "noisetexture", Json::object(), "Noise Texture"), 0, 1);
    });
    add("gen_text", "Generated media: Titles & Text 'Reno 123' over a clip", [&](Test& t, Project& p) {
        t.fontDependent = true;
        Json st = params({{"TextColor", {1, 1, 0, 1}}});
        Json s = Json::object();
        s["s"] = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Arial;}}\\f0\\fs48 Reno 123\\par}";
        st["params"]["Text"] = s;
        track(p, TrackType::Video, "Text");
        track(p, TrackType::Video, "Clip");
        event(p, p.tracks[0], generated(p, t, "titlesandtext", st, "Titles & Text"), 0, 1);
        event(p, p.tracks[1], media(p, "clip_a.avi"), 0, 1);
    });

    // ---- VEGAS's OpenFX effects (Reno runs its stand-ins on Linux where it has them)
    auto fx = [&](const std::string& name, const std::string& title, const Json& state, const char* where = "event") {
        add("fx_" + name + (std::string(where) == "event" ? "" : std::string("_") + where), title, [=](Test& t, Project& p) {
            track(p, TrackType::Video);
            Event& e = event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.2);
            FxSlot s = fxSlot(p, t, name, state);
            if (std::string(where) == "event") e.fx.push_back(s);
            else if (std::string(where) == "track") p.tracks[0].fx.push_back(s);
            else p.videoOut.push_back(s);
        });
    };
    fx("gaussianblur", "Gaussian Blur 0.02 / 0.05", params({{"HorizontalRange", {0.02}}, {"VerticalRange", {0.05}}}));
    fx("gaussianblur", "Gaussian Blur on the track", params({{"HorizontalRange", {0.03}}, {"VerticalRange", {0.03}}}), "track");
    fx("gaussianblur", "Gaussian Blur on the video output", params({{"HorizontalRange", {0.03}}, {"VerticalRange", {0.03}}}), "output");
    fx("brightnessandcontrast", "Brightness and Contrast +0.2 / +0.3", params({{"Brightness", {0.2}}, {"Contrast", {0.3}}}));
    fx("colorcorrector", "Color Corrector: saturation 1.6, gamma 1.3", params({{"Saturation", {1.6}}, {"Gamma", {1.3}}}));
    fx("hsladjust", "HSL Adjust: hue +0.25, saturation 0.5", params({{"AddToHue", {0.25}}, {"Saturation", {0.5}}}));
    fx("levels", "Levels: input 0.1 .. 0.8, gamma 1.4", params({{"InputStart", {0.1}}, {"InputEnd", {0.8}}, {"Gamma", {1.4}}}));
    fx("invert", "Invert", Json::object());
    fx("blackandwhite", "Black and White", Json::object());
    fx("sepia", "Sepia (defaults)", Json::object());
    fx("sharpen", "Sharpen 0.8", params({{"Amount", {0.8}}}));
    fx("pixelate", "Pixelate 0.05", params({{"HorizontalPixelization", {0.05}}, {"VerticalPixelization", {0.05}}}));
    fx("mirror", "Mirror (defaults)", Json::object());
    fx("glow", "Glow (defaults)", Json::object());
    fx("border", "Border: solid, 0.08", params({{"Type", {2}}, {"Size", {0.08}}}));
    fx("colorbalance", "Color Balance: red +0.4 in the midtones", params({{"Red", {0.4}}, {"Type", {1}}}));
    fx("chromakeyer", "Chroma Keyer (defaults) over black", Json::object());
    fx("cookiecutter", "Cookie Cutter (defaults)", Json::object());
    fx("radialblur", "Radial Blur (defaults)", Json::object());
    fx("vignette", "Vignette (defaults)", Json::object());
    add("fx_chain_keyframes", "Brightness animated -0.5 -> +0.5 then Gaussian Blur (a chain with keyframes)", [&](Test& t, Project& p) {
        track(p, TrackType::Video);
        Event& e = event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.6);
        Json st = params({{"Brightness", {-0.5}}});
        Json k = Json::array();
        Json k0 = Json::array(); k0.push(0.0); k0.push(-0.5);
        Json k1 = Json::array(); k1.push(1.5 * 25); k1.push(0.5);
        k.push(k0); k.push(k1);
        st["params"]["Brightness"]["k"] = k;
        e.fx.push_back(fxSlot(p, t, "brightnessandcontrast", st));
        e.fx.push_back(fxSlot(p, t, "gaussianblur", params({{"HorizontalRange", {0.02}}, {"VerticalRange", {0.02}}})));
    });

    // ---- a nested project (built by VEGAS first: its id sorts first)
    add("nested_a_inner", "Nested: the inner project (two clips, a cross-fade)", [&](Test&, Project& p) {
        auto [v, a] = va(p);
        auto e1 = clip(p, *v, *a, "clip_a.avi", 0, 1.2);
        auto e2 = clip(p, *v, *a, "clip_b.avi", 0.8, 1.2);
        e1.first->fadeOut = e1.second->fadeOut = 0.4;
        e2.first->fadeIn = e2.second->fadeIn = 0.4;
    });

    // ---- frame rates and frame sizes
    add("fps_2997_from_25", "A 25 fps clip in a 29.97 fps project", [&](Test&, Project& p) {
        p.settings.fpsNum = 30000; p.settings.fpsDen = 1001;
        auto [v, a] = va(p);
        clip(p, *v, *a, "clip_a.avi", 0, 2.0);
    });
    add("fps_5994", "A 59.94 fps clip in a 59.94 fps project", [&](Test&, Project& p) {
        p.settings.fpsNum = 60000; p.settings.fpsDen = 1001;
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_5994.avi"), 0, 1.5);
    });
    add("fps_120", "A 25 fps clip in a 120 fps project", [&](Test&, Project& p) {
        p.settings.fpsNum = 120;
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.0);
    });
    add("fps_24_from_5994", "A 59.94 fps clip in a 24 fps project", [&](Test&, Project& p) {
        p.settings.fpsNum = 24;
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_5994.avi"), 0, 1.5);
    });
    add("h264_decode", "An H.264 + AAC 640x360 29.97 file in a matching project (decoding, colour conversion)", [&](Test&, Project& p) {
        p = newProject(640, 360, 30000, 1001);
        auto [v, a] = va(p);
        clip(p, *v, *a, "clip_h264.mp4", 0, 2.0, 0.5);
    });
    add("size_odd", "An odd frame size (333x187): the clip scaled to fit", [&](Test&, Project& p) {
        p = newProject(333, 187);
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.0);
    });
    add("size_portrait", "A portrait frame (180x320): the clip letterboxed", [&](Test&, Project& p) {
        p = newProject(180, 320);
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.0);
    });
    add("quality_good", "Render quality Good (not Best): a zoomed pan/crop", [&](Test&, Project& p) {
        p.settings.renderQuality = "good";
        track(p, TrackType::Video);
        event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.0).panCrop = {{0, 0.5, 0.5, 0.37, 0.37, 0, 0}};
    });
    add("pixel_32bit", "32-bit floating point project: a blur and a bright brightness", [&](Test& t, Project& p) {
        p.settings.pixelFormat = "32bit";
        track(p, TrackType::Video);
        Event& e = event(p, p.tracks[0], media(p, "clip_a.avi"), 0, 1.0);
        e.fx.push_back(fxSlot(p, t, "brightnessandcontrast", params({{"Brightness", {0.6}}})));
        e.fx.push_back(fxSlot(p, t, "gaussianblur", params({{"HorizontalRange", {0.02}}, {"VerticalRange", {0.02}}})));
    });
    return all;
}

// the outer nested project needs the inner one's .veg (VEGAS's own, in the bridge; Reno's .reno.veg here)
static Test nestedOuter(const std::string& innerVeg)
{
    Test t;
    t.id = "nested_b_outer";
    t.title = "Nested: a VEGAS project as media, zoomed by pan/crop, over a clip";
    t.p = newProject();
    Project& p = t.p;
    track(p, TrackType::Video, "Nested");
    track(p, TrackType::Video, "Under");
    track(p, TrackType::Audio, "Nested sound");
    const uint64_t m = media(p, innerVeg);
    Event& e = event(p, p.tracks[0], m, 0, 1.8, 0.1);
    e.panCrop = {{0, 0.5, 0.5, 0.8, 0.8, 0, 0}};
    event(p, p.tracks[1], media(p, "clip_b.avi"), 0, 1.8);
    event(p, p.tracks[2], m, 0, 1.8, 0.1);
    return t;
}

// ------------------------------------------------------------------------------------------ writing
static Json jobFor(const Test& t, bool renoVeg)
{
    const std::string src = renoVeg ? "suite/" + t.id + ".reno.veg" : "suite/" + t.id + ".veg";
    const std::string tag = renoVeg ? "renoveg" : "vegas";
    Json actions = Json::array();
    auto act = [&](std::initializer_list<std::pair<const char*, Json>> kv) {
        Json a = Json::object();
        for (const auto& x : kv) a[x.first] = x.second;
        actions.push(a);
    };
    if (!renoVeg) act({{"do", "build"}, {"reno", "suite/" + t.id + ".reno"}, {"save_as", src}});
    act({{"do", "open"}, {"project", src}});
    act({{"do", "project_info"}, {"out", "info/" + t.id + "." + tag + ".json"}});
    if (!renoVeg) act({{"do", "export_reno"}, {"out", "info/" + t.id + ".vegas.reno"}});
    Json render = Json::object();
    render["do"] = "render";
    if (t.audioOnly) {
        render["out"] = "renders/" + tag + "/" + t.id + ".wav";
        Json r = Json::array(); r.push("Wave (Microsoft)"); r.push("Wave"); r.push("wav");
        Json tp = Json::array(); tp.push("project"); tp.push("48,000 Hz, 16 Bit, Stereo, PCM"); tp.push("Default Template");
        render["renderer"] = r;
        render["template"] = tp;
    } else {
        render["out"] = "renders/" + tag + "/" + t.id + ".avi";
        Json r = Json::array(); r.push("Video for Windows"); r.push("AVI");
        Json tp = Json::array(); tp.push("Default Template (uncompressed)"); tp.push("project");
        render["renderer"] = r;
        render["template"] = tp;
    }
    actions.push(render);
    Json j = Json::object();
    j["tag"] = tag + ":" + t.id;
    j["timeout"] = 900;
    j["continue_on_error"] = true;
    j["actions"] = actions;
    return j;
}

int main(int argc, char** argv)
{
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--win-root" && i + 1 < argc) g_win = argv[++i];
        else if (a == "--only" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t b = 0;
            while (b <= s.size()) {
                size_t e = s.find(',', b);
                if (e == std::string::npos) e = s.size();
                if (e > b) only.insert(s.substr(b, e - b));
                b = e + 1;
            }
        } else if (a[0] != '-' && g_root.empty()) g_root = a;
        else { std::fprintf(stderr, "usage: makesuite <bridge folder> [--win-root DIR] [--only id,id]\n"); return 2; }
    }
    if (g_root.empty()) { std::fprintf(stderr, "usage: makesuite <bridge folder> [--win-root DIR] [--only id,id]\n"); return 2; }
    while (!g_win.empty() && (g_win.back() == '\\' || g_win.back() == '/')) g_win.pop_back();
    for (const auto& b : ofx::builtinInfos()) g_builtins.insert(b.id);
    fs::create_directories(g_root + "/suite");
    fs::create_directories(g_root + "/jobs");
    fs::create_directories(g_root + "/jobs_renoveg");

    // first: what this VEGAS has (render templates; plug-ins with their parameters - names VEGAS shows and uses)
    writeText(g_root + "/jobs/000_setup.job.json",
              "{\n  \"tag\": \"setup\",\n  \"timeout\": 600,\n  \"continue_on_error\": true,\n  \"actions\": [\n"
              "    {\"do\": \"ping\"},\n    {\"do\": \"list_templates\", \"out\": \"info/templates.json\"},\n"
              "    {\"do\": \"list_plugins\", \"out\": \"info/plugins.json\"}\n  ]\n}\n");
    writeText(g_root + "/jobs/000b_plugin_params.job.json",
              "{\n  \"tag\": \"plugin_params\",\n  \"timeout\": 1800,\n  \"actions\": [\n"
              "    {\"do\": \"list_plugins\", \"params\": true, \"out\": \"info/plugins_params.json\"}\n  ]\n}\n");
    std::vector<Test> tests = makeTests();
    Json manifest = Json::object();
    manifest["win_root"] = g_win;
    manifest["made_by"] = "tests/compare/makesuite.cpp";
    Json list = Json::array();
    int n = 0, written = 0;
    auto emit = [&](Test& t) {
        ++n;
        if (!only.empty() && !only.count(t.id)) return;
        std::vector<std::string> warnings;
        std::string err;
        // the Reno project file for VEGAS to build
        if (!t.p.save(g_root + "/suite/" + t.id + ".reno", &err)) { std::fprintf(stderr, "%s: %s\n", t.id.c_str(), err.c_str()); return; }
        // Reno's own .veg of it (the nested outer's points at the inner's .reno.veg)
        Project copy = t.p;
        for (auto& m : copy.media)
            if (m.name == "nested_a_inner.veg") { m.name = "nested_a_inner.reno.veg"; m.path = g_win + "\\suite\\nested_a_inner.reno.veg"; m.info.path = m.path; }
        VegasSaveResult r = saveVegasProject(copy, g_root + "/suite/" + t.id + ".reno.veg");
        if (!r.ok) std::fprintf(stderr, "%s: Reno's .veg: %s\n", t.id.c_str(), r.error.c_str());
        std::remove((g_root + "/suite/" + t.id + ".reno.veg.bak").c_str());
        char num[16];
        std::snprintf(num, sizeof num, "%03d", n);
        writeText(g_root + "/jobs/" + num + "_" + t.id + ".job.json", jobFor(t, false).dump(2) + "\n");
        writeText(g_root + "/jobs_renoveg/" + num + "_" + t.id + ".job.json", jobFor(t, true).dump(2) + "\n");
        Json e = Json::object();
        e["id"] = t.id;
        e["title"] = t.title;
        e["seconds"] = t.p.length();
        e["width"] = t.p.settings.width;
        e["height"] = t.p.settings.height;
        e["fps"] = t.p.settings.fps();
        e["fps_num"] = t.p.settings.fpsNum;
        e["fps_den"] = t.p.settings.fpsDen;
        e["sample_rate"] = t.p.settings.sampleRate;
        e["audio_only"] = t.audioOnly;
        bool hasAudio = false;
        for (const auto& tr : t.p.tracks) if (tr.type == TrackType::Audio && !tr.events.empty()) hasAudio = true;
        e["has_audio"] = hasAudio;
        e["font_dependent"] = t.fontDependent;
        Json needs = Json::array(), missing = Json::array();
        std::set<std::string> seen;
        for (const auto& id : t.needs) {
            if (!seen.insert(id).second) continue;
            needs.push(id);
            if (!g_builtins.count(id)) missing.push(id);
        }
        e["needs"] = needs;
        e["reno_linux_missing"] = missing;        // plug-ins Reno cannot run here: the test is marked, not failed
        e["reno_linux_ok"] = missing.size() == 0;
        e["reno_veg_warnings"] = (int)r.warnings.size();
        list.push(e);
        ++written;
    };
    for (auto& t : tests) emit(t);
    Test outer = nestedOuter("nested_a_inner.veg");
    emit(outer);
    manifest["tests"] = list;
    writeText(g_root + "/manifest.json", manifest.dump(2) + "\n");
    std::printf("%d tests written to %s (%d in all)\n", written, g_root.c_str(), n);
    return 0;
}
