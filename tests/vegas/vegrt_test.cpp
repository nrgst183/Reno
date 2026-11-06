// vegrt_test: what Reno's .veg writer must not lose on a round trip (Reno project -> .veg -> Reno), with projects made here
// (no sample data needed). Found by tools/vegcov_compare.py roundtrip.
//
//  1. The ruler's time format (header word 0x08) is written for a new project and read back, for every format VEGAS has.
//  2. An audio plug-in whose VEGAS name is known from the .reno it came from (AudioPluginDesc::vegasClsid, also read from
//     the export script's "vegas" note) is saved even when no sample project has used it, with its settings; a reproduced
//     eFX effect with no saved settings is saved with its defaults; the name survives a further save and a .reno save.
//  3. OpenFX settings of an effect the file has no record of are written with the layout VEGAS's own records show
//     (whole numbers or doubles), and non-whole values as doubles; settings that could be either are reported, not guessed.
//  4. A cross-fade's out-curve that Reno leaves to the mixer (FadeIn.ReciprocalCurve not given) is written as the curve
//     the mixer plays (the earlier event's fade-out curve).
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegfxlayouts.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/engine/timefmt.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace reno;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
static bool check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " - ", detail.c_str()); std::fflush(stdout); }
    return ok;
}

static std::string g_dir;

// Saves 'p' as a .veg in the scratch folder and reads it back.
static bool roundTrip(Project& p, const std::string& name, Project& back, std::vector<std::string>* warnings = nullptr)
{
    const std::string path = g_dir + "/" + name + ".veg";
    fs::remove(path);
    const VegasSaveResult r = saveVegasProject(p, path);
    if (warnings) *warnings = r.warnings;
    if (!r.ok) { check(false, name + ": save", r.error); return false; }
    back = Project();
    const VegasImportResult ir = importVegasProject(path, back);
    if (!ir.ok) { check(false, name + ": reload", ir.error); return false; }
    return true;
}

static bool mentions(const std::vector<std::string>& w, const char* text)
{
    for (const auto& s : w) if (s.find(text) != std::string::npos) return true;
    return false;
}

// A project with one audio track and two overlapping events of a media file that is not there.
static Project makeProject()
{
    Project p;
    MediaItem m;
    m.id = p.newId();
    m.path = "C:\\media\\tone.wav";
    m.name = "tone.wav";
    m.offline = true;
    m.info.hasAudio = true;
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    for (int i = 0; i < 2; ++i) {
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.start = i * 3.0;
        e.length = 5.0;
        t.events.push_back(e);
    }
    t.sortEvents();
    p.tracks.push_back(t);
    return p;
}

static FxSlot vst2(Project& p, const char* name, const char* uid, const std::string& state, const std::string& clsidHex)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.name = name;
    s.audio.format = "VST2";
    s.audio.uid = uid;
    s.audio.name = name;
    s.audio.vegasClsid = clsidHex;
    if (!state.empty()) s.state = state;
    return s;
}

int main(int argc, char** argv)
{
    g_dir = argc > 1 ? argv[1] : (fs::temp_directory_path() / "vegrt_test").string();
    std::error_code ec;
    fs::create_directories(g_dir, ec);

    // ---- 1. ruler format
    {
        for (const char* f : {"time", "timecode", "smpte30"}) {
            Project p = makeProject();
            p.settings.rulerFormat = f;
            Project back;
            std::vector<std::string> w;
            if (roundTrip(p, std::string("ruler_") + f, back, &w)) {
                check(back.settings.rulerFormat == f, std::string("ruler format ") + f, back.settings.rulerFormat);
                check(!mentions(w, "ruler"), std::string("no warning for ruler format ") + f);
            }
        }
        // saving again without a change keeps it, and a change is written
        Project p = makeProject();
        p.settings.rulerFormat = "time";
        Project back;
        if (roundTrip(p, "ruler_again", back)) {
            Project again;
            back.settings.rulerFormat = "smpte30";
            if (roundTrip(back, "ruler_again2", again)) check(again.settings.rulerFormat == "smpte30", "ruler format changed after a read");
        }
    }

    // ---- 2. audio plug-ins named by the .veg or .reno they came from
    {
        // 'WAtn' and four bytes VEGAS worked out here, then the tail every VST CLSID has
        const std::string clsid = "6e74415701020304" "0269000" "7e95d36fa";
        check(clsid.size() == 32, "test clsid has 16 bytes");
        Project p = makeProject();
        // some settings: three parameters as 32-bit floats
        const float vals[3] = {0.25f, 0.5f, 1.0f};
        const std::string state = "P:" + base64Encode(vals, sizeof vals);
        p.tracks[0].fx.push_back(vst2(p, "Test Tuner", "1463907438", state, clsid));
        // the same plug-in without VEGAS's name for it cannot be written (no way to name it): said, not guessed
        p.tracks[0].fx.push_back(vst2(p, "Other Plug", "1463907439", state, ""));
        // a reproduced eFX effect with no settings: its defaults
        p.tracks[0].fx.push_back(vst2(p, "eFX Limiter", "1297625665", "", ""));
        Event* e0 = &p.tracks[0].events[0];
        (void)e0;
        Project back;
        std::vector<std::string> w;
        if (roundTrip(p, "vstclsid", back, &w)) {
            const FxChain& c = back.tracks[0].fx;
            check(c.size() == 2, "chain length (the unnamed plug-in is left out)", std::to_string(c.size()));
            if (c.size() == 2) {
                check(c[0].audio.uid == "1463907438" && c[0].name == "Test Tuner", "named plug-in comes back");
                check(c[0].audio.vegasClsid == clsid, "its VEGAS name comes back", c[0].audio.vegasClsid);
                check(c[0].state.isString() && c[0].state.asString() == state, "its settings come back");
                check(c[1].audio.uid == "1297625665", "eFX effect comes back");
                check(c[1].state.isString() && c[1].state.asString().compare(0, 2, "P:") == 0, "eFX effect has its default program");
            }
            check(mentions(w, "Other Plug"), "the plug-in that could not be named is reported");
            // saving the reread project again keeps both (the .veg is the origin now) and a .reno keeps the name
            Project again;
            if (roundTrip(back, "vstclsid2", again)) check(again.tracks[0].fx.size() == 2, "second save keeps the chain");
            const std::string reno = g_dir + "/vstclsid.reno";
            std::string err;
            Project loaded;
            check(back.save(reno, &err) && loaded.load(reno, &err), "reno save and load", err);
            if (loaded.tracks.size() == 1 && loaded.tracks[0].fx.size() == 2) {
                check(loaded.tracks[0].fx[0].audio.vegasClsid == clsid, ".reno keeps the VEGAS name");
                loaded.tracks[0].fx[0].veg.reset();
                Project third;
                if (roundTrip(loaded, "vstclsid3", third)) check(third.tracks[0].fx.size() == 2, "a .reno's plug-ins are saved to a .veg");
            }
        }
        // the export script's note in a .reno: "vegas": {"classId": "<GUID>"} (the GUID's first fields are little-endian)
        {
            Project q = makeProject();
            q.tracks[0].fx.push_back(vst2(q, "Test Tuner", "1463907438", state, ""));
            const std::string reno = g_dir + "/vstnote.reno";
            std::string err;
            check(q.save(reno, &err), "save for the note test", err);
            std::ifstream in(reno, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            const size_t at = text.find("\"Test Tuner\"");
            const size_t bp = at == std::string::npos ? at : text.find("\"bypass\"", at);
            check(bp != std::string::npos, "found the slot in the saved .reno");
            if (bp != std::string::npos) {
                text.insert(bp, "\"vegas\": {\"classId\": \"5741746e-0201-0403-0269-0007e95d36fa\"}, ");
                std::ofstream(reno, std::ios::binary) << text;
                Project loaded;
                check(loaded.load(reno, &err), "load with the note", err);
                if (!loaded.tracks.empty() && !loaded.tracks[0].fx.empty())
                    check(loaded.tracks[0].fx[0].audio.vegasClsid == "6e74415701020304" "0269000" "7e95d36fa", "the note gives the CLSID bytes",
                          loaded.tracks[0].fx[0].audio.vegasClsid);
            }
        }
    }

    // ---- 3. OpenFX settings of an effect with no record in the file
    {
        check(veg::knownFxSettingIsWhole("com.example.noeffect", "Amount") == -1, "unknown setting is unknown");
        Project p = makeProject();
        FxSlot s;
        s.id = p.newId();
        s.kind = "ofx";
        s.pluginId = "com.example.noeffect";
        s.name = "Example";
        Json params = Json::object();
        auto num = [](double v) { Json e = Json::object(); Json a = Json::array(); a.push(Json(v)); e["v"] = a; return e; };
        params["Amount"] = num(0.25);       // not a whole number: a double
        params["Count"] = num(3);           // could be either: reported
        Json st = Json::object();
        st["plugin"] = s.pluginId;
        st["params"] = params;
        s.state = st;
        // an effect VEGAS ships: the table of VEGAS's own records says which settings are whole numbers
        FxSlot s2;
        s2.id = p.newId();
        s2.kind = "ofx";
        s2.pluginId = "com.vegascreativesoftware:brightnessandcontrast";
        Json params2 = Json::object();
        params2["Brightness"] = num(0.1);
        params2["Percent"] = num(1);            // a double that happens to be whole
        params2["ReferenceMode"] = num(1);      // a choice
        Json st2 = Json::object();
        st2["plugin"] = s2.pluginId;
        st2["params"] = params2;
        s2.state = st2;
        check(veg::knownFxSettingIsWhole(s2.pluginId, "ReferenceMode") == 1 && veg::knownFxSettingIsWhole(s2.pluginId, "Percent") == 0, "table knows the settings");
        Track vt;
        vt.id = p.newId();
        vt.type = TrackType::Video;
        vt.fx.push_back(s);
        vt.fx.push_back(s2);
        p.tracks.insert(p.tracks.begin(), vt);
        Project back;
        std::vector<std::string> w;
        if (roundTrip(p, "ofxlayout", back, &w)) {
            const FxChain& c = back.tracks[0].fx;
            check(c.size() == 2, "effects come back");
            if (c.size() == 2) {
                const Json& q = c[1].state.get("params");
                check(q.has("Brightness") && q.get("Brightness").get("v").at(0).asDouble() == 0.1 && !q.get("Brightness").has("i"), "table: a double");
                check(q.has("Percent") && q.get("Percent").get("v").at(0).asDouble() == 1 && !q.get("Percent").has("i"), "table: a double with a whole value");
                check(q.has("ReferenceMode") && q.get("ReferenceMode").get("v").at(0).asDouble() == 1 && q.get("ReferenceMode").has("i"), "table: whole numbers");
                const Json& ps = c[0].state.get("params");
                check(ps.has("Amount") && ps.get("Amount").get("v").size() == 1 && ps.get("Amount").get("v").at(0).asDouble() == 0.25,
                      "non-whole setting is written as a double");
                check(!ps.has("Count"), "a setting that could be either is left out");
            }
            check(mentions(w, "Count"), "and reported");
        }
    }

    // ---- 4. cross-fade out-curve left to the mixer
    {
        Project p = makeProject();
        Track& t = p.tracks[0];
        Event a = t.events[0], b = t.events[1];
        a.fadeOutCurve = CurveSlow;
        a.fadeOut = 2.0;
        b.fadeIn = 2.0;
        b.fadeInCurve = CurveSharp;
        b.fadeInRecipCurve = -1;
        t.events = CowVec<Event>();
        t.events.push_back(a);
        t.events.push_back(b);
        check(effectiveFadeInRecipCurve(t, t.events[1]) == CurveSlow, "effective curve is the earlier event's fade-out");
        Project back;
        if (roundTrip(p, "recip", back)) {
            const auto& ev = back.tracks[0].events;
            check(ev.size() == 2 && ev[1].fadeInRecipCurve == CurveSlow, "written as the curve the mixer plays");
        }
        // with no event under it: the mirror of the fade-in
        Project q = makeProject();
        q.tracks[0].events[1].start = 20;
        q.tracks[0].events[1].fadeInCurve = CurveFast;
        q.tracks[0].events[1].fadeInRecipCurve = -1;
        check(effectiveFadeInRecipCurve(q.tracks[0], q.tracks[0].events[1]) == CurveSlow, "alone: mirror of the fade-in");
    }

    // 5. A Pan/Crop key's angle is in radians, positive turning the Pan/Crop window clockwise on the screen (as Reno's degrees do):
    //    the stored value is +0.5 for a key of 28.6479 degrees, and comes back as the same degrees. (Checked against VEGAS's own
    //    pictures of a key made through its scripting interface with 0.5 rad.)
    {
        Project p;
        MediaItem m;
        m.id = p.newId();
        m.path = "C:\\media\\pic.png";
        m.name = "pic.png";
        m.offline = true;
        m.info.hasVideo = true;
        m.info.isStill = true;
        m.info.width = 640;
        m.info.height = 360;
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 5.0;
        PanCropKey k;
        k.cx = 0.5; k.cy = 0.5; k.w = 0.5; k.h = 0.5;
        k.rotation = 0.5 * 180.0 / 3.14159265358979323846;
        e.panCrop.push_back(k);
        t.events.push_back(e);
        p.tracks.push_back(t);
        Project back;
        if (roundTrip(p, "pancroprot", back)) {
            const auto& pk = back.tracks[0].events[0].panCrop;
            check(pk.size() == 1 && std::fabs(pk[0].rotation - k.rotation) < 1e-6, "Pan/Crop angle comes back", pk.empty() ? "no key" : std::to_string(pk[0].rotation));
            std::ifstream in(g_dir + "/pancroprot.veg", std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string bytes = ss.str();
            const size_t at = bytes.find("POSK");
            bool good = false;
            double stored = 0;
            if (at != std::string::npos && at + 68 <= bytes.size()) {
                std::memcpy(&stored, bytes.data() + at + 60, 8);
                good = std::fabs(stored - 0.5) < 1e-9;
            }
            check(good, "Pan/Crop angle is stored in radians, positive clockwise", std::to_string(stored));
        }
    }

    // 6. Second coverage run: the ruler's Microseconds and Nanoseconds, HDR mode and 360 output, a grandchild's nesting level
    {
        for (const char* f : {"nanoseconds", "microseconds"}) {
            Project p = makeProject();
            p.settings.rulerFormat = f;
            Project back;
            if (roundTrip(p, std::string("ruler_") + f, back)) check(back.settings.rulerFormat == f, std::string("ruler format ") + f, back.settings.rulerFormat);
        }
        check(timeFormatFromScript(16) == "nanoseconds" && timeFormatFromScript(17) == "microseconds", "script values of the two formats");
        {
            ProjectSettings s;
            double v = 0;
            check(formatTime(s, 1.5, "microseconds") == "1500000" && formatTime(s, 1.5, "nanoseconds") == "1500000000", "written in their units");
            check(parseTimeText(s, "2500000", "microseconds", &v) && std::fabs(v - 2.5) < 1e-12, "read in microseconds");
        }
        Project p = makeProject();
        p.settings.hdrMode = "hlg";
        p.settings.output360 = true;
        Project back;
        std::vector<std::string> w;
        if (roundTrip(p, "hdr360", back, &w)) {
            check(back.settings.hdrMode == "hlg" && back.settings.output360, "HDR mode and 360 output come back", back.settings.hdrMode);
            check(!mentions(w, "HDR") && !mentions(w, "360"), "and are not reported as lost");
            back.settings.hdrMode = "hdr10";
            back.settings.output360 = false;
            Project again;
            if (roundTrip(back, "hdr360b", again)) check(again.settings.hdrMode == "hdr10" && !again.settings.output360, "a change after a read is written");
        }
        // three video tracks nested one inside the other: the levels 0, 1, 2 survive a save
        Project q = makeProject();
        q.tracks.clear();
        for (int i = 0; i < 4; ++i) {
            Track t;
            t.id = q.newId();
            t.type = TrackType::Video;
            t.name = "V" + std::to_string(i);
            t.compositeLevel = i < 3 ? i : 0;
            q.tracks.push_back(t);
        }
        Project qb;
        if (roundTrip(q, "nesting", qb) && qb.tracks.size() == 4) {
            check(qb.tracks[0].compositeLevel == 0 && qb.tracks[1].compositeLevel == 1 && qb.tracks[2].compositeLevel == 2 && qb.tracks[3].compositeLevel == 0,
                  "nesting levels 0,1,2,0 come back");
        } else {
            check(false, "nesting round trip", std::to_string(qb.tracks.size()));
        }
        // an image sequence keeps the frame rate it was opened at (a 12-byte record ahead of the stream records)
        {
            const std::string dir = g_dir + "/seq";
            fs::create_directories(dir, ec);
            for (int i = 0; i < 4; ++i) std::ofstream(dir + "/shot_000" + std::to_string(i) + ".png", std::ios::binary) << "x";
            Project sp;
            MediaItem sm;
            sm.id = sp.newId();
            sm.path = dir + "/shot_0000.png";
            sm.name = "shot_0000 - shot_0003";
            sm.sequenceFps = 12;
            sm.info.hasVideo = true;
            sm.info.width = 64;
            sm.info.height = 48;
            sp.media.push_back(sm);
            Track st;
            st.id = sp.newId();
            st.type = TrackType::Video;
            Event se;
            se.id = sp.newId();
            se.mediaId = sm.id;
            se.length = 1.0;
            st.events.push_back(se);
            sp.tracks.push_back(st);
            Project sb;
            if (roundTrip(sp, "sequence", sb) && !sb.media.empty()) check(std::fabs(sb.media[0].sequenceFps - 12.0) < 1e-9, "image sequence rate comes back", std::to_string(sb.media[0].sequenceFps));
        }
    }

    std::printf("vegrt_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
