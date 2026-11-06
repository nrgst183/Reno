// vegcovproj_test: the project properties, track and event settings found by comparing a VEGAS Pro 18 coverage run
// (tools/vegas/RenoCoverage.cs, tools/vegcov_compare.py) with what Reno read. Everything is made here: a project is
// saved as a .veg, the bytes are checked at the places VEGAS keeps each setting (found by changing one setting at a
// time in VEGAS), some are changed as VEGAS would have written them, and the file is read back.
//
//  1. Project properties: ruler format, start time and beats, field order, motion blur, pixel format, output rotation,
//     audio CD and LFE settings, master bus mode.
//  2. The sample rate and bit depth are the mixer's, not the header word (which VEGAS leaves at 48000).
//  3. A track's fade colours are stored blue, green, red, alpha.
//  4. An event's snap offset, a take's place among the event's takes, and an image sequence as media.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
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

static std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
static void putFile(const std::string& path, const std::string& bytes)
{
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

static bool saveAndLoad(Project& p, const std::string& name, Project& back, std::string* bytes = nullptr, std::vector<std::string>* warnings = nullptr)
{
    const std::string path = g_dir + "/" + name + ".veg";
    fs::remove(path);
    const VegasSaveResult r = saveVegasProject(p, path);
    if (warnings) *warnings = r.warnings;
    if (!r.ok) { check(false, name + ": save", r.error); return false; }
    if (bytes) *bytes = readFile(path);
    back = Project();
    const VegasImportResult ir = importVegasProject(path, back);
    if (!ir.ok) { check(false, name + ": reload", ir.error); return false; }
    return true;
}

static bool reload(const std::string& name, const std::string& bytes, Project& back)
{
    const std::string path = g_dir + "/" + name + ".veg";
    putFile(path, bytes);
    back = Project();
    const VegasImportResult ir = importVegasProject(path, back);
    if (!ir.ok) check(false, name + ": reload", ir.error);
    return ir.ok;
}

// A TrackMotionList (VEGAS keeps one only for a track with track motion) holding a TrackMotion chunk made here, put at the end of
// the first track's list; the sizes of the lists around it grow with it.
static std::string withTrackMotion(const std::string& file, const std::string& motion)
{
    veg::ChunkTree tree;
    veg::readChunkTree(veg::Bytes((const uint8_t*)file.data(), file.size()), tree);
    const veg::Chunk* tracks = tree.root.child(veg::G::Tracks);
    const veg::Chunk* track = tracks ? tracks->child(veg::G::Track) : nullptr;
    if (!track) return std::string();
    auto pad8 = [](std::string s) { while (s.size() % 8) s += '\0'; return s; };
    auto u64 = [](uint64_t v) { return std::string((const char*)&v, 8); };
    const std::string chunk = pad8(veg::guidBytes(veg::G::TrackMotion) + u64((24 + motion.size() + 7) & ~(size_t)7) + motion);   // (the size counts the padding)
    const std::string list = pad8(veg::guidBytes(veg::G::List) + u64(40 + chunk.size()) + veg::guidBytes(veg::G::TrackMotionList) + chunk);
    uint64_t trackSize = 0;
    std::memcpy(&trackSize, file.data() + track->fileOffset + 16, 8);
    const size_t at = track->fileOffset + (size_t)((trackSize + 7) & ~7ull);
    std::string out = file.substr(0, at) + list + file.substr(at);
    for (size_t o : {tree.root.fileOffset, tracks->fileOffset, track->fileOffset}) {
        uint64_t sz = 0;
        std::memcpy(&sz, out.data() + o + 16, 8);
        sz += list.size();
        std::memcpy(&out[o + 16], &sz, 8);
    }
    return out;
}

// the header chunk's bytes inside the file (offset of its data, size), found through the chunk tree
static bool findChunk(const std::string& bytes, const char* guid, size_t& at, size_t& size)
{
    veg::ChunkTree tree;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), tree);
    bool found = false;
    std::function<void(const veg::Chunk&)> walk = [&](const veg::Chunk& c) {
        for (const auto& k : c.kids) {
            if (!found && !k.isList && k.id == guid) {
                found = true;
                at = k.fileOffset + 24;
                size = k.data.size();
            }
            if (k.isList) walk(k);
        }
    };
    walk(tree.root);
    return found;
}

static uint32_t u32(const std::string& b, size_t o) { uint32_t v = 0; std::memcpy(&v, b.data() + o, 4); return v; }
static uint16_t u16(const std::string& b, size_t o) { uint16_t v = 0; std::memcpy(&v, b.data() + o, 2); return v; }
static int64_t i64(const std::string& b, size_t o) { int64_t v = 0; std::memcpy(&v, b.data() + o, 8); return v; }
static float f32(const std::string& b, size_t o) { float v = 0; std::memcpy(&v, b.data() + o, 4); return v; }
static void put32(std::string& b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }

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
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 5.0;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

int main(int argc, char** argv)
{
    g_dir = argc > 1 ? argv[1] : (fs::temp_directory_path() / "vegcovproj_test").string();
    std::error_code ec;
    fs::create_directories(g_dir, ec);

    // ---- 1. project properties
    {
        Project p = makeProject();
        ProjectSettings& s = p.settings;
        s.width = 1080;
        s.height = 1920;
        s.rulerFormat = "smpteDrop";
        s.rulerStart = 0.5;
        s.beatsPerMeasure = 3;
        s.noteGetsBeat = 8;
        s.tempo = 97.5;
        s.fieldOrder = "lower";
        s.motionBlur = "box";
        s.outputRotation = 270;
        s.cdFirstTrack = 5;
        s.cdUpc = "012345678905";
        s.lfeLowpass = false;
        s.lfeCutoff = 100;
        s.audioChannels = 6;
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "props", back, &bytes)) {
            const ProjectSettings& b = back.settings;
            check(b.rulerFormat == "smpteDrop", "ruler format SMPTE drop", b.rulerFormat);
            check(std::fabs(b.rulerStart - 0.5) < 1e-9, "ruler start time", std::to_string(b.rulerStart));
            check(b.beatsPerMeasure == 3 && b.noteGetsBeat == 8, "beats per measure and note that gets the beat");
            check(std::fabs(b.tempo - 97.5) < 1e-9, "tempo");
            check(b.fieldOrder == "lower", "field order", b.fieldOrder);
            check(b.motionBlur == "box", "motion blur type", b.motionBlur);
            check(b.outputRotation == 270, "output rotation", std::to_string(b.outputRotation));
            check(b.cdFirstTrack == 5, "audio CD first track");
            check(b.cdUpc == "012345678905", "audio CD UPC", b.cdUpc);
            check(!b.lfeLowpass && b.lfeCutoff == 100, "LFE low-pass");
            check(b.audioChannels == 6, "master bus mode 5.1");
            check(b.width == 1080 && b.height == 1920, "picture size not swapped by the rotation");
            // the places VEGAS keeps them (found by changing one setting at a time)
            size_t at = 0, sz = 0;
            if (check(findChunk(bytes, veg::G::Header, at, sz), "header chunk found")) {
                check(u32(bytes, at + 8) == 8, "ruler word for SMPTE drop is 8");
                check(i64(bytes, at + 0x20) == -5000000, "ruler start stored negated");
                check(u16(bytes, at + 0x28) == 8 && u16(bytes, at + 0x2a) == 3, "note at 0x28, beats per measure at 0x2a");
                check(u32(bytes, at + 0x70) == 5, "CD first track at 0x70");
                check(u32(bytes, at + 0x74) == 0 && u32(bytes, at + 0x78) == 100, "LFE switch and cutoff at 0x74 and 0x78");
                // the project's video block follows the four texts
                size_t t = u32(bytes, at);
                for (int i = 0; i < 4; ++i) {
                    while (!(bytes[at + t] == 0 && bytes[at + t + 1] == 0 && (t % 2) == 0)) t += 2;
                    t += 2;
                }
                check(u32(bytes, at + t + 0x34) == 2, "field order at +0x34 of the video block (2 lower)");
                check(u32(bytes, at + t + 0xf0) == 2, "motion blur at +0xf0 of the video block (2 box)");
                check(u32(bytes, at + t + 0x130) == 3, "output rotation at +0x130 of the video block (3 counterclockwise)");
            }
            if (check(findChunk(bytes, veg::G::Mixer, at, sz), "mixer chunk found"))
                check(u32(bytes, at + 0xc) == 2 && u32(bytes, at + 0x10) == 6, "surround master bus mode at 0xc and 0x10 of the mixer");

            // saving it again unchanged keeps every one
            Project again;
            if (saveAndLoad(back, "props_again", again)) {
                check(again.settings.rulerStart == back.settings.rulerStart && again.settings.beatsPerMeasure == 3 &&
                          again.settings.outputRotation == 270 && again.settings.cdUpc == "012345678905" && again.settings.audioChannels == 6,
                      "settings survive a second save");
            }
        }
        // a 32-bit float (video levels) project
        Project q = makeProject();
        q.settings.pixelFormat = "32bit";
        q.settings.pixelRange = "video";
        Project qb;
        if (saveAndLoad(q, "props_float", qb)) check(qb.settings.pixelFormat == "32bit" && qb.settings.pixelRange == "video", "32-bit float pixel format");
    }

    // ---- 2. sample rate and bit depth
    {
        Project p = makeProject();
        p.settings.sampleRate = 44100;
        p.settings.audioBits = 24;
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "rate", back, &bytes)) {
            check(back.settings.sampleRate == 44100 && back.settings.audioBits == 24, "sample rate and bit depth read back");
            // VEGAS leaves the header's word at 48000 when the rate is set in the properties
            size_t at = 0, sz = 0;
            if (findChunk(bytes, veg::G::Header, at, sz)) {
                put32(bytes, at + 0x0c, 48000);
                Project other;
                if (reload("rate_header48000", bytes, other)) check(other.settings.sampleRate == 44100, "the mixer's sample rate wins over the header's", std::to_string(other.settings.sampleRate));
            }
        }
    }

    // ---- 3. fade colours: blue, green, red, alpha
    {
        Project p;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.fadeTop = 0xFFFF8000u;       // orange, opaque
        t.fadeBottom = 0x800040FFu;    // blue-ish, half transparent
        p.tracks.push_back(t);
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "fadecolour", back, &bytes) && check(back.tracks.size() == 1, "video track read back")) {
            check(back.tracks[0].fadeTop == 0xFFFF8000u, "top fade colour read back", std::to_string(back.tracks[0].fadeTop));
            check(back.tracks[0].fadeBottom == 0x800040FFu, "bottom fade colour read back", std::to_string(back.tracks[0].fadeBottom));
            size_t at = 0, sz = 0;
            if (findChunk(bytes, veg::G::VideoTrackData, at, sz)) {
                check(std::fabs(f32(bytes, at + 0x30) - 0.0f) < 1e-6 && std::fabs(f32(bytes, at + 0x34) - 128 / 255.0f) < 1e-6 &&
                          std::fabs(f32(bytes, at + 0x38) - 1.0f) < 1e-6 && std::fabs(f32(bytes, at + 0x3c) - 1.0f) < 1e-6,
                      "the top colour is stored blue, green, red, alpha");
            }
        }
    }

    // ---- 4. snap offset, active take, image sequence
    {
        Project p = makeProject();
        MediaItem m2 = p.media[0];
        m2.id = p.newId();
        m2.path = "C:\\media\\other.wav";
        m2.name = "other.wav";
        p.media.push_back(m2);
        MediaItem m3 = p.media[0];
        m3.id = p.newId();
        m3.path = "C:\\media\\third.wav";
        m3.name = "third.wav";
        p.media.push_back(m3);
        Event& e = p.tracks[0].events[0];
        e.snapOffset = 0.25;
        // takes in VEGAS's order: tone, other, third; the active one is the third
        Take a;
        a.mediaId = p.media[0].id;
        a.name = "First take";
        Take b;
        b.mediaId = p.media[1].id;
        b.name = "Second take";
        e.mediaId = p.media[2].id;
        e.takes = {a, b};
        e.activeTake = 2;
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "events", back, &bytes) && check(back.tracks.size() == 1 && back.tracks[0].events.size() == 1, "event read back")) {
            const Event& r = back.tracks[0].events[0];
            check(std::fabs(r.snapOffset - 0.25) < 1e-9, "snap offset read back", std::to_string(r.snapOffset));
            check(r.takes.size() == 2 && r.activeTake == 2, "active take is the third of three", std::to_string(r.activeTake));
            const MediaItem* am = back.findMedia(r.mediaId);
            check(am && am->path.find("third.wav") != std::string::npos, "the active take plays the third file");
            check(!r.takes.empty() && r.takes[0].name == "First take", "a take keeps its name");
        }
    }
    {
        // an image sequence: three numbered pictures in the scratch folder
        const std::string dir = g_dir + "/seq";
        fs::create_directories(dir, ec);
        for (int i = 0; i < 3; ++i) {
            char name[64];
            std::snprintf(name, sizeof name, "%s/shot_%04d.png", dir.c_str(), i);
            putFile(name, "x");
        }
        Project p;
        MediaItem m;
        m.id = p.newId();
        m.path = dir + "/shot_0000.png";
        m.name = "shot_####.png";
        m.sequenceFps = 25;
        m.offline = true;
        m.info.hasVideo = true;
        m.info.isStill = true;
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 2.0;
        t.events.push_back(e);
        p.tracks.push_back(t);
        Project back;
        std::string bytes;
        std::vector<std::string> w;
        if (saveAndLoad(p, "sequence", back, &bytes, &w)) {
            // (the paths are UTF-16)
            std::string wide;
            for (char c : std::string("META:\\Image Sequence\\{")) { wide += c; wide += '\0'; }
            check(bytes.find(wide) != std::string::npos, "the media is written as an image sequence");
            check(back.tracks.size() == 1 && back.tracks[0].events.size() == 1, "an event of the sequence is read back");
            if (!back.media.empty()) {
                check(back.media[0].sequenceFps > 0, "the media is an image sequence again");
                check(back.media[0].path.find("shot_0000.png") != std::string::npos, "its first picture is the path", back.media[0].path);
                check(back.media[0].name == "shot_0000 - shot_0002", "VEGAS's name for the sequence", back.media[0].name);
            }
        }
    }

    // 5. An older DirectX generator (named by class GUID, no OpenFX record) is read as the built-in of the same name
    {
        Project p;
        MediaItem m;
        m.id = p.newId();
        m.generated = true;
        m.name = "VEGAS Credit Roll";
        m.generator.id = p.newId();
        m.generator.kind = "ofx";
        m.generator.pluginId = "com.vegascreativesoftware:creditroll";
        m.info.hasVideo = true;
        m.info.width = 640;
        m.info.height = 360;
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 2.0;
        t.events.push_back(e);
        p.tracks.push_back(t);
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "gen_dx", back, &bytes)) {
            check(back.tracks.size() == 1 && back.tracks[0].events.size() == 1, "an OpenFX generator's event is read back");
            // make it look like VEGAS's DirectX one: the path names the class GUID (same length, padded) and the OpenFX record's id is gone
            auto wide = [](const std::string& a) { std::string w; for (char c : a) { w += c; w += '\0'; } return w; };
            const std::string from = wide("{Svfx:com.vegascreativesoftware:creditroll}");
            std::string to = wide("{02FD3B60-986B-11D4-9F6F-00485481BD31}");
            to.resize(from.size(), ' ');
            for (size_t i = to.size() - 5 * 2; i < to.size(); i += 2) { to[i] = ' '; to[i + 1] = '\0'; }
            const size_t at = bytes.find(from);
            check(at != std::string::npos, "the generator's path is in the file");
            if (at != std::string::npos) {
                bytes.replace(at, from.size(), to);
                static const unsigned char ofx[16] = {0x41, 0x1a, 0xfc, 0xa2, 0xf6, 0x01, 0x0e, 0x46, 0xae, 0x5a, 0x2b, 0xe4, 0xaf, 0x57, 0xb9, 0x2e};
                const size_t g = bytes.find(std::string((const char*)ofx, 16));
                check(g != std::string::npos, "the OpenFX record is in the file");
                if (g != std::string::npos) std::memset(&bytes[g], 0, 16);
                Project legacy;
                if (reload("gen_dx", bytes, legacy)) {
                    check(legacy.tracks.size() == 1 && legacy.tracks[0].events.size() == 1, "the DirectX generator's event is read");
                    check(!legacy.media.empty() && legacy.media[0].generated && legacy.media[0].generator.pluginId == "com.vegascreativesoftware:creditroll",
                          "it is the Credit Roll built-in", legacy.media.empty() ? "" : legacy.media[0].generator.pluginId);
                    // saved again unchanged, the record stays as it was
                    const std::string path2 = g_dir + "/gen_dx.veg";      // (the project name in the file is the file name)
                    fs::remove(path2);
                    const VegasSaveResult r2 = saveVegasProject(legacy, path2);
                    check(r2.ok && readFile(path2) == bytes, "saved again unchanged, it is the same bytes");
                }
            }
        }
    }

    // 6. Bypass Motion Blur (bit 0x100 of the video settings' word 0x0c), and Track Motion read from the file's keys
    {
        Project p;
        p.settings.width = 640;
        p.settings.height = 360;
        MediaItem m;
        m.id = p.newId();
        m.path = "C:\\media\\clip.mp4";
        m.name = "clip.mp4";
        m.offline = true;
        m.info.hasVideo = true;
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.bypassMotionBlur = true;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 2.0;
        t.events.push_back(e);
        p.tracks.push_back(t);
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "motion", back, &bytes)) {
            size_t at = 0, sz = 0;
            check(back.tracks.size() == 1 && back.tracks[0].bypassMotionBlur, "Bypass Motion Blur is read back");
            if (check(findChunk(bytes, veg::G::VideoTrackData, at, sz), "the video track settings are found"))
                check((u32(bytes, at + 0x0c) & 0x100) != 0, "Bypass Motion Blur is bit 0x100 of the word at 0x0c");
            // a track motion chunk as VEGAS keeps it: one key each of motion, shadow and glow, with VEGAS's numbers put in
            {
                std::string m(0x24 + (4 + 0xdc) + (4 + 0xd8) + (4 + 0xd8), '\0');
                put32(m, 0x00, 0x24);
                put32(m, 0x04, 1);
                put32(m, 0x1c, 3);
                put32(m, 0x24, 1);
                put32(m, 0x28, 0xdc);
                put32(m, 0x28 + 0xdc, 1);
                put32(m, 0x28 + 0xdc + 4, 0xd8);
                put32(m, 0x28 + 0xdc + 4 + 0xd8, 1);
                put32(m, 0x28 + 0xdc + 4 + 0xd8 + 4, 0xd8);
                bytes = withTrackMotion(bytes, m);
                check(!bytes.empty(), "the track motion chunk is put in");
            }
            size_t atm = 0, szm = 0;
            if (!bytes.empty() && check(findChunk(bytes, veg::G::TrackMotion, atm, szm), "the track motion chunk is found")) {
                at = atm;
                sz = szm;
                auto putD = [&](size_t o, double v) { std::memcpy(&bytes[o], &v, 8); };
                auto putF = [&](size_t o, float v) { std::memcpy(&bytes[o], &v, 4); };
                const double asp = 640.0 / 360.0, pi = 3.14159265358979323846;
                put32(bytes, at + 0x04, u32(bytes, at + 0x04) | 0x08 | 0x10);      // shadow and glow on
                putD(at + 0x14, asp);
                const size_t k0 = at + 0x28, ks = at + 0x108, kg = at + 0x1e4;      // (each key from its size word)
                put32(bytes, k0 + 0x04, 4);                                         // smooth
                putD(k0 + 0x30, 0.5);                                               // 90 px right of the middle
                putD(k0 + 0x38, -0.25);                                             // 45 px down
                putD(k0 + 0x50, 320.0 / 360.0);                                     // 320 px wide
                putD(k0 + 0x58, 0.5);                                               // 180 px high
                putD(k0 + 0x80, 0.35 * pi / 180);                                   // turned 0.35 degrees
                putD(k0 + 0xb0, 0.4);                                               // smoothness 40
                putD(k0 + 0x40, 60.0 / 180);                                        // z 60 px
                putD(k0 + 0x70, 0.4 * pi / 180);                                    // rotation about x 0.4 degrees
                putD(k0 + 0x60, 2.0 / 360);                                         // depth 2 px
                putD(ks + 0x30, 9.0 / 180);
                putD(ks + 0x38, -9.0 / 180);
                putF(ks + 0xb0, 0.0f); putF(ks + 0xb4, 0.0f); putF(ks + 0xb8, 1.0f); putF(ks + 0xbc, 1.0f);   // red
                putD(ks + 0xc0, 0.003);
                putD(ks + 0xc8, 0.006);
                putD(kg + 0x50, 1.05);
                putF(kg + 0xb0, 0.0f); putF(kg + 0xb4, 1.0f); putF(kg + 0xb8, 1.0f); putF(kg + 0xbc, 0.5f);   // yellow, half
                Project moved;
                if (reload("motion", bytes, moved) && check(moved.tracks.size() == 1 && moved.tracks[0].motion.size() == 1, "the motion key is read")) {
                    const MotionKey& k = moved.tracks[0].motion[0];
                    check(std::fabs(k.x - (0.5 + 90.0 / 640)) < 1e-9 && std::fabs(k.y - 0.625) < 1e-9, "position", std::to_string(k.x) + "," + std::to_string(k.y));
                    check(std::fabs(k.w - 0.5) < 1e-9 && std::fabs(k.h - 0.5) < 1e-9, "size", std::to_string(k.w) + "," + std::to_string(k.h));
                    check(std::fabs(k.rotation - 0.35) < 1e-9, "rotation (degrees, clockwise positive)", std::to_string(k.rotation));
                    check(std::fabs(k.smoothness - 40) < 1e-9 && k.curve != 0, "smoothness and type");
                    check(std::fabs(k.z - 60.0 / 640) < 1e-9 && std::fabs(k.rotX - 0.4) < 1e-9 && std::fabs(k.d - 2.0 / 18) < 1e-9, "3D fields");
                    check(moved.tracks[0].shadow2d.size() == 1 && moved.tracks[0].glow2d.size() == 1, "shadow and glow keys are read");
                    if (moved.tracks[0].shadow2d.size() == 1) {
                        const GlowKey& g = moved.tracks[0].shadow2d[0];
                        check(g.color == 0xFFFF0000u && std::fabs(g.blur - 0.003) < 1e-12 && std::fabs(g.x - 0.05 / (2 * asp)) < 1e-9, "shadow key", std::to_string(g.color));
                    }
                    if (moved.tracks[0].glow2d.size() == 1)
                        check(moved.tracks[0].glow2d[0].color == 0x80FFFF00u && std::fabs(moved.tracks[0].glow2d[0].w - 1.05) < 1e-12, "glow key");
                    // saved again unchanged, the chunk stays as it was
                    const std::string path2 = g_dir + "/motion.veg";
                    const std::string before = readFile(path2);
                    const VegasSaveResult r2 = saveVegasProject(moved, path2);
                    check(r2.ok && readFile(path2) == before, "saved again unchanged, the file is the same bytes");
                    check(r2.warnings.empty(), "no warning for track motion that is as read", r2.warnings.empty() ? "" : r2.warnings[0]);
                }
            }
        }
    }

    // 7. Envelopes of the master, the buses and the video output
    {
        // an envelope record as the points chunk keeps it: 0x38 header and 0x18 per point
        auto record = [](uint32_t type, float lo, float hi, float neutral, const std::vector<std::pair<int64_t, float>>& pts, int32_t curve) {
            std::string r(0x38 + pts.size() * 0x18, '\0');
            put32(r, 0, 0x38);
            put32(r, 8, type);
            std::memcpy(&r[0x0c], &lo, 4);
            std::memcpy(&r[0x10], &hi, 4);
            std::memcpy(&r[0x14], &neutral, 4);
            put32(r, 0x18, (uint32_t)pts.size());
            put32(r, 0x34, 0x18);
            for (size_t i = 0; i < pts.size(); ++i) {
                std::memcpy(&r[0x38 + i * 0x18], &pts[i].first, 8);
                std::memcpy(&r[0x38 + i * 0x18 + 8], &pts[i].second, 4);
                put32(r, 0x38 + i * 0x18 + 12, (uint32_t)curve);
            }
            return r;
        };
        // inside a bus record: some settings, a text of odd length, the count, the records
        std::string bus = std::string(0x40, '\x07') + "a" + std::string(3, '\0') + std::string(4, '\0');
        const std::string r1 = record(0, 0, 2, 1, {{0, 1.0f}, {50000000, 0.5f}}, 1);
        const std::string r2 = record(31, 0, 1, 1, {{0, 1.0f}}, 0);
        std::string count(4, '\0');
        put32(count, 0, 2);
        std::string rec = bus + count + r1 + r2 + std::string(0x48, '\0');
        std::vector<veg::EnvelopeRecord> got;
        const bool ok = veg::readBusEnvelopes(veg::Bytes((const uint8_t*)rec.data(), rec.size()), got);
        check(ok && got.size() == 2, "a bus record's envelopes are found after settings of any length");
        if (ok && got.size() == 2) {
            check(got[0].type == veg::EnvVolume && got[0].points.size() == 2 && std::fabs(got[0].points[1].time - 5.0) < 1e-9 && got[0].points[1].value == 0.5, "its volume envelope");
            check(got[1].type == veg::EnvMute && got[1].points.size() == 1, "its mute envelope");
        }
        std::vector<veg::EnvelopeRecord> none;
        check(!veg::readBusEnvelopes(veg::Bytes((const uint8_t*)bus.data(), bus.size()), none) && none.empty(), "no envelopes where there are none");

        // the video output's envelopes: a list of its own after the other top-level lists
        Project p = makeProject();
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "videoenv", back, &bytes)) {
            auto pad8 = [](std::string x) { while (x.size() % 8) x += '\0'; return x; };
            auto u64 = [](uint64_t v) { return std::string((const char*)&v, 8); };
            auto chunk = [&](const char* id, const std::string& data) { return pad8(veg::guidBytes(id) + u64((24 + data.size() + 7) & ~(size_t)7) + data); };
            const std::string header = std::string(0x20, '\0');
            const std::string kids = chunk("84701b5b-6863-11d2-86fd-00c04f8edb8a", header) +
                                     chunk("84701b5c-6863-11d2-86fd-00c04f8edb8a", record(33, 1, 8, 1, {{0, 1.0f}, {10000000, 2.75f}}, 1));
            const std::string list = pad8(veg::guidBytes(veg::G::List) + u64(40 + kids.size()) + veg::guidBytes("f0ca0912-3506-496f-bdbf-7807976210d2") + kids);
            std::string withEnv = bytes + list;
            uint64_t rootSize = 0;
            std::memcpy(&rootSize, withEnv.data() + 16, 8);
            rootSize += list.size();
            std::memcpy(&withEnv[16], &rootSize, 8);
            Project env;
            if (reload("videoenv", withEnv, env)) {
                check(env.videoEnvelopes.size() == 1 && env.videoEnvelopes[0].kind == "supersample" && env.videoEnvelopes[0].points.size() == 2 &&
                          env.videoEnvelopes[0].points[1].value == 2.75f,
                      "the video output's supersampling envelope is read", env.videoEnvelopes.empty() ? "none" : env.videoEnvelopes[0].kind);
                const std::string path2 = g_dir + "/videoenv.veg";
                const VegasSaveResult r2 = saveVegasProject(env, path2);
                check(r2.ok && readFile(path2) == withEnv, "saved again unchanged, it is the same bytes");
                check(r2.warnings.empty(), "no warning for envelopes that are as read", r2.warnings.empty() ? "" : r2.warnings[0]);
            }
        }
    }

    // 8. The video output's fade colours, and Event Pan/Crop's rotation sense and smoothness
    {
        Project p;
        p.settings.width = 640;
        p.settings.height = 360;
        MediaItem m;
        m.id = p.newId();
        m.path = "C:\\media\\clip.mp4";
        m.name = "clip.mp4";
        m.offline = true;
        m.info.hasVideo = true;
        m.info.width = 640;
        m.info.height = 360;
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 4.0;
        PanCropKey k0, k1;
        k0.time = 0;
        k0.cx = 0.5; k0.cy = 0.5; k0.w = 1.0; k0.h = 1.0;
        k1.time = 2.0;
        k1.cx = 0.4; k1.cy = 0.5; k1.w = 0.5; k1.h = 0.5;
        k1.rotation = 0.2 * 180.0 / 3.14159265358979323846;       // 0.2 radians: turned clockwise
        k1.smoothness = 50;
        k1.curve = 4;
        e.panCrop = {k0, k1};
        t.events.push_back(e);
        p.tracks.push_back(t);
        p.videoFadeTop = 0xFF336699;        // (alpha, red, green, blue)
        p.videoFadeBottom = 0xFFE61A1A;
        Project back;
        std::string bytes;
        if (saveAndLoad(p, "pancrop", back, &bytes)) {
            check(back.videoFadeTop == 0xFF336699u && back.videoFadeBottom == 0xFFE61A1Au, "the video output's fade colours are read back",
                  std::to_string(back.videoFadeTop) + "," + std::to_string(back.videoFadeBottom));
            // the keys' bytes: the second key's rotation is +0.2 and its last double is the smoothness 0..1
            std::vector<size_t> at;
            for (size_t i = bytes.find("POSK"); i != std::string::npos; i = bytes.find("POSK", i + 4)) at.push_back(i + 8);
            if (check(at.size() == 2, "two pan/crop keys are saved")) {
                double rot = 0, smooth = 0;
                std::memcpy(&rot, &bytes[at[1] + 52], 8);
                std::memcpy(&smooth, &bytes[at[1] + 76], 8);
                check(std::fabs(rot - 0.2) < 1e-9, "a positive rotation is positive in the file", std::to_string(rot));
                check(std::fabs(smooth - 0.5) < 1e-12, "the key's smoothness is the last double, 0..1", std::to_string(smooth));
            }
            if (back.tracks.size() == 1 && back.tracks[0].events.size() == 1 && back.tracks[0].events[0].panCrop.size() == 2) {
                const PanCropKey& r = back.tracks[0].events[0].panCrop[1];
                check(std::fabs(r.rotation - k1.rotation) < 1e-9 && std::fabs(r.smoothness - 50) < 1e-9 && r.curve == 4, "rotation, smoothness and type are read back");
            } else {
                check(false, "the pan/crop keys are read back");
            }
        }
    }

    std::printf("vegcovproj_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
