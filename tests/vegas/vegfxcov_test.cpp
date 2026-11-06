// vegfxcov_test: what the coverage run of VEGAS Pro 18 showed about effects in a .veg, with the bytes made here (no user data):
//  1. transitions (OpenFX) and the Video Output FX list are written and read back, also unchanged on a second save;
//  2. a transition of VEGAS's older kind (a record with a class GUID) is read as Reno's transition of that name and kept
//     byte for byte while it is not changed; replacing it writes a new OpenFX record;
//  3. an older effect's bypass (0x01) and "before pan/crop" (0x40, or 0x80 as the older samples have it) bits, and the flag
//     set again by the writer;
//  4. presets are kept (FxSlot::preset) and written for a new record.
// Usage: vegfxcov_test        (needs no arguments)
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/veglegacyfx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace reno;

static int g_pass = 0, g_fail = 0;
static bool check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
    return ok;
}

static FxSlot newOfx(Project& p, const std::string& plugin, const std::string& preset)
{
    FxSlot f;
    f.id = p.newId();
    f.kind = "ofx";
    f.pluginId = plugin;
    f.preset = preset;
    Json params = Json::object();
    Json a = Json::object();
    Json v = Json::array();
    v.push(0.375);
    a["v"] = v;
    a["i"] = false;
    params["Amount"] = a;
    Json b = Json::object();
    Json bv = Json::array();
    bv.push(2.0);
    b["v"] = bv;
    b["i"] = true;
    params["Shape"] = b;
    f.state = Json::object();
    f.state["plugin"] = plugin;
    f.state["params"] = params;
    return f;
}

static uint64_t addMedia(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = "clip";
    m.info.path = path;
    m.info.hasVideo = true;
    m.info.width = 1920;
    m.info.height = 1080;
    m.info.duration = 60;
    p.media.push_back(m);
    return m.id;
}

static Event newEvent(Project& p, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = 1.0;
    return e;
}

static Project made()
{
    Project p;
    p.settings.width = 1280;
    p.settings.height = 720;
    p.settings.fpsNum = 25;
    p.settings.fpsDen = 1;
    const uint64_t clip = addMedia(p, "C:\\Reno test\\clip.mp4");
    Track v;
    v.id = p.newId();
    v.type = TrackType::Video;
    v.name = "Video";
    Event e1 = newEvent(p, clip, 0, 3);
    Event e2 = newEvent(p, clip, 2, 3);
    e2.fadeIn = 1.0;
    e2.transition = newOfx(p, "com.vegascreativesoftware:iris", "Square, In, Center");
    v.events = {e1, e2};
    p.tracks = {v};
    p.videoOut.push_back(newOfx(p, "com.vegascreativesoftware:blackandwhite", "(Default)"));
    p.videoOut.push_back(newOfx(p, "com.vegascreativesoftware:sharpen", ""));
    p.videoOut[1].bypass = true;
    return p;
}

static bool load(const std::string& bytes, Project& q, std::vector<std::string>* notes = nullptr)
{
    const VegasImportResult r = importVegasData(bytes, "C:\\Reno test\\t.veg", q);
    if (notes) *notes = r.notes;
    return check(r.ok, "the file reads (" + r.error + ")");
}

static const std::string kOfx("\x30\x00\x00\x00\x41\x1a\xfc\xa2\xf6\x01\x0e\x46\xae\x5a\x2b\xe4\xaf\x57\xb9\x2e", 20);

int main()
{
    // ---- 1. a transition and the video output's effects
    Project p = made();
    std::vector<std::string> warn;
    const std::string bytes = writeVegasData(p, "C:\\Reno test\\t.veg", &warn);
    for (const auto& w : warn) std::printf("   warning: %s\n", w.c_str());
    if (const char* keep = std::getenv("VEGFXCOV_KEEP")) {        // keep the file for looking at it (vegdump --tree)
        if (FILE* f = std::fopen(keep, "wb")) { std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f); }
    }
    Project q;
    if (!load(bytes, q)) return 1;
    check(q.tracks.size() == 1 && q.tracks[0].events.size() == 2, "two events read back");
    const Event& e2 = q.tracks[0].events[1];
    check(q.tracks[0].events[0].transition.kind.empty(), "the first event has no transition");
    check(e2.transition.kind == "ofx" && e2.transition.pluginId == "com.vegascreativesoftware:iris", "the transition is read back: " + e2.transition.pluginId);
    check(e2.transition.preset == "Square, In, Center", "its preset is kept: " + e2.transition.preset);
    check(e2.transition.state.get("params").get("Amount").get("v").at(0).asDouble() == 0.375, "its Amount is read back");
    check(e2.transition.state.get("params").get("Shape").get("v").at(0).asDouble() == 2.0, "its Shape is read back");
    check(!e2.transition.beforePanCrop, "a transition is not 'before pan/crop'");
    check(q.videoOut.size() == 2, "two video output effects read back");
    if (q.videoOut.size() == 2) {
        check(q.videoOut[0].pluginId == "com.vegascreativesoftware:blackandwhite" && q.videoOut[0].preset == "(Default)", "video output effect 1");
        check(q.videoOut[1].pluginId == "com.vegascreativesoftware:sharpen" && q.videoOut[1].bypass, "video output effect 2 (bypassed)");
        check(q.videoOut[1].preset == "(Default)", "an effect with no preset is written as (Default): " + q.videoOut[1].preset);
    }
    // saved again unchanged: the same bytes
    const std::string again = writeVegasData(q, "C:\\Reno test\\t.veg", nullptr);
    check(again == bytes, "a second save of what was read is the same file");
    // removed: the list is empty / the transition gone
    Project r = q;
    r.tracks[0].events[1].transition = FxSlot();
    r.videoOut.clear();
    const std::string removed = writeVegasData(r, "C:\\Reno test\\t.veg", nullptr);
    Project q2;
    if (load(removed, q2)) {
        check(q2.tracks[0].events[1].transition.kind.empty(), "a removed transition is gone");
        check(q2.videoOut.empty(), "removed video output effects are gone");
    }
    // changed: the new settings are written
    r = q;
    {
        Json nv = Json::array();
        nv.push(0.75);
        Json amount = r.tracks[0].events[1].transition.state.get("params").get("Amount");
        amount["v"] = nv;
        r.tracks[0].events[1].transition.state["params"]["Amount"] = amount;
    }
    Project q3;
    if (load(writeVegasData(r, "C:\\Reno test\\t.veg", nullptr), q3))
        check(q3.tracks[0].events[1].transition.state.get("params").get("Amount").get("v").at(0).asDouble() == 0.75, "a changed transition setting is saved");

    // ---- 2. a transition of the older kind: the OpenFX record in the file is overwritten, in place, by a record of the older kind
    //         (class GUID of VEGAS 3D Blinds at +4, bytes after it arbitrary)
    std::string old = bytes;
    const size_t at = old.rfind(kOfx);      // the video output's effects come first in the file, the transition is the last record
    if (!check(at != std::string::npos, "the OpenFX transition record is in the file")) return 1;
    const uint8_t blinds[16] = {0x52, 0xa5, 0x43, 0xc8, 0xeb, 0xaa, 0x1c, 0x41, 0x84, 0x81, 0xcf, 0x4c, 0x52, 0xa3, 0x36, 0xe3};
    std::memcpy(&old[at], "\x38\x00\x00\x00", 4);
    std::memcpy(&old[at + 4], blinds, 16);
    Project o1;
    std::vector<std::string> notes;
    if (load(old, o1, &notes)) {
        const FxSlot& t = o1.tracks[0].events[1].transition;
        check(t.kind == "ofx" && t.pluginId == "com.vegascreativesoftware:3dblinds", "an older transition is read as Reno's of that name: " + t.pluginId);
        const std::string saved = writeVegasData(o1, "C:\\Reno test\\t.veg", nullptr);
        check(saved == old, "an older transition not changed is written back as it was");
        Project o2 = o1;
        o2.tracks[0].events[1].transition.pluginId = "com.vegascreativesoftware:iris";
        o2.tracks[0].events[1].transition.state = Json::object();
        Project o3;
        if (load(writeVegasData(o2, "C:\\Reno test\\t.veg", nullptr), o3))
            check(o3.tracks[0].events[1].transition.pluginId == "com.vegascreativesoftware:iris", "another transition in its place is written as OpenFX");
        bool noted = false;
        for (const auto& n : notes) noted = noted || n.find("older kind") != std::string::npos;
        check(noted, "the older transition is mentioned in the notes");
    }

    // ---- 3. the bits of an older effect record
    veg::Bytes none;
    std::string rec(0x40, '\0');
    uint32_t flags = 0x30e;
    std::memcpy(&rec[0x2c], &flags, 4);
    auto before = [&](uint32_t f) {
        std::memcpy(&rec[0x2c], &f, 4);
        return veg::recordBeforePanCrop(veg::Bytes((const uint8_t*)rec.data(), rec.size()), 0);
    };
    check(!before(0x30e) && !before(0x31c), "no 'before pan/crop' in a plain record");
    check(before(0x34f) && before(0x345), "0x40 is 'before pan/crop' (the coverage run's bypassed + before effects hold 0x41)");
    check(before(0x38c), "0x80 still counts (the older samples)");
    std::string r2 = rec;
    flags = 0x30e;
    std::memcpy(&r2[0x2c], &flags, 4);
    veg::setRecordBeforePanCrop(r2, true);
    uint32_t got = 0;
    std::memcpy(&got, &r2[0x2c], 4);
    check(got == 0x34e, "switching it on sets 0x40");
    veg::setRecordBeforePanCrop(r2, false);
    std::memcpy(&got, &r2[0x2c], 4);
    check(got == 0x30e, "switching it off clears it");
    flags = 0x38c;
    std::memcpy(&r2[0x2c], &flags, 4);
    veg::setRecordBeforePanCrop(r2, true);
    std::memcpy(&got, &r2[0x2c], 4);
    check(got == 0x38c, "a record that has 0x80 and stays 'before pan/crop' is not touched");

    // ---- Threshold and Film Grain: older records whose numbers are 32-bit floats (Threshold) or three doubles (made here, like VEGAS Pro 18's)
    {
        auto u32 = [](std::string& s, uint32_t v) { s.append((const char*)&v, 4); };
        auto record = [&](const unsigned char* guid, const char16_t* name, size_t nameChars, const std::string& block) {
            std::string r;
            u32(r, 0x38);
            r.append((const char*)guid, 16);
            u32(r, (uint32_t)block.size());
            u32(r, 1);
            u32(r, 0); u32(r, 0); u32(r, 0);
            u32(r, 2);
            u32(r, 0x304);
            u32(r, 0);
            u32(r, (uint32_t)((nameChars + 1) * 2));
            for (size_t i = 0; i <= nameChars; ++i) { const char16_t c = i < nameChars ? name[i] : 0; r.append((const char*)&c, 2); }
            r += block;
            u32(r, 0x1c);
            r.append(8 + 12, '\0');
            u32(r, (uint32_t)block.size());
            r += block;
            return r;
        };
        // Threshold {C3062BE8-E69D-11D1-9AA9-00A0C99B12C5}, the level 0.25 as a float
        static const unsigned char thr[16] = {0xe8, 0x2b, 0x06, 0xc3, 0x9d, 0xe6, 0xd1, 0x11, 0x9a, 0xa9, 0x00, 0xa0, 0xc9, 0x9b, 0x12, 0xc5};
        const float level = 0.25f;
        std::string tb((const char*)&level, 4);
        std::string rec = record(thr, u"25 %", 4, tb);
        rec.append(8 - rec.size() % 8, '\0');
        check(veg::isLegacyFxAt(veg::Bytes((const uint8_t*)rec.data(), rec.size()), 0), "a Threshold record is recognised");
        veg::LegacyFxRecord lr;
        if (check(veg::readLegacyFx(veg::Bytes((const uint8_t*)rec.data(), rec.size()), 0, lr), "a Threshold record is read") && lr.statik.size() == 1) {
            check(lr.statik[0] == 0.25 && lr.keys.size() == 1 && lr.keys[0].v.size() == 1 && lr.keys[0].v[0] == 0.25, "its level is a float 0.25");
            check(veg::legacyPluginId(lr) == "com.vegascreativesoftware:threshold", "it is Reno's Threshold");
            Json st;
            std::vector<std::string> problems;
            check(veg::legacyFxState(lr, 30, st, problems) && st.get("params").get("Threshold").get("v").at(0).asDouble() == 0.25, "the state has Threshold 0.25");
            const std::string back = veg::writeLegacyFx(veg::Bytes((const uint8_t*)rec.data(), rec.size()), 0, lr);
            check(back == rec.substr(0, back.size()), "it is written back as it was");
        }
        // Film Grain {B78CBE2C-9292-4C7C-9682-5D8A71CC984B}: Amount, Granularity, Chromaticity
        static const unsigned char grain[16] = {0x2c, 0xbe, 0x8c, 0xb7, 0x92, 0x92, 0x7c, 0x4c, 0x96, 0x82, 0x5d, 0x8a, 0x71, 0xcc, 0x98, 0x4b};
        const double g3[3] = {0.312, 0.14, 0.0};
        std::string gb((const char*)g3, 24);
        std::string grec = record(grain, u"Subtle", 6, gb);
        grec.append(8 - grec.size() % 8, '\0');
        veg::LegacyFxRecord gr;
        if (check(veg::readLegacyFx(veg::Bytes((const uint8_t*)grec.data(), grec.size()), 0, gr), "a Film Grain record is read")) {
            Json st;
            std::vector<std::string> problems;
            check(veg::legacyFxState(gr, 30, st, problems) && st.get("params").get("Amount").get("v").at(0).asDouble() == 0.312 &&
                      st.get("params").get("Granularity").get("v").at(0).asDouble() == 0.14,
                  "Film Grain's Amount and Granularity are read");
        }
        // TV Simulator {B892D510-9487-4022-B261-738123B55E47}: the coverage run's "TV Look" record; Line sync and Vertical sync are the OpenFX numbers
        static const unsigned char tv[16] = {0x10, 0xd5, 0x92, 0xb8, 0x87, 0x94, 0x22, 0x40, 0xb2, 0x61, 0x73, 0x81, 0x23, 0xb5, 0x5e, 0x47};
        const double t8[8] = {1.0, 0.316, 0.66, 1.0, 1.0, 0.15, 0.9, 0.0};
        std::string tb8((const char*)t8, 64);
        std::string trec = record(tv, u"TV Look", 7, tb8);
        trec.append(8 - trec.size() % 8, '\0');
        veg::LegacyFxRecord tr;
        if (check(veg::readLegacyFx(veg::Bytes((const uint8_t*)trec.data(), trec.size()), 0, tr), "a TV Simulator record is read")) {
            Json st;
            std::vector<std::string> problems;
            check(veg::legacyFxState(tr, 30, st, problems), "its state is made");
            const Json& pr = st.get("params");
            check(pr.get("DetailZoom").get("v").at(0).asDouble() == 0.0 && pr.get("LineSync").get("v").at(0).asDouble() == 1.0 &&
                      pr.get("VerticalSync").get("v").at(0).asDouble() == 1.0 && pr.get("Interlacing").get("v").at(0).asDouble() == 0.66,
                  "TV Look: no zoom, line and vertical sync 1 (as the OpenFX preset), interlacing 0.66");
        }
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
