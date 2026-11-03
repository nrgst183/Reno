// vegplugenv_test: VEGAS's type 301 envelopes (automation of one setting of a VST plug-in in a track's effect chain).
// Everything is made here (no user data): a project with Reno's eFX Limiter and eFX Reverb on an audio track, an envelope
// on the Reverb's Mix, saved as a .veg, read back, played, edited and saved again.
//
//   vegplugenv_test
//
//  1. The envelope is saved as type 301: setting index 9 (Mix), place 1 in the chain, the Reverb's CLSID, percent values
//     (0..100, neutral 50 = Mix's default 0.5), and reads back as an "fx" envelope on the Reverb's slot, named "Mix".
//  2. Saving what was read changes no byte; an edited point is saved (same plug-in fields); swapping the two effects
//     changes the place; an envelope that names a plug-in the chain does not hold is not saved (and is said).
//  3. A type 301 envelope naming a CLSID the chain does not hold is kept as it was (hidden, written back byte for byte).
//  4. The engine plays it: the Reverb's Mix follows the envelope (value / 100, VEGAS's curves).
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/engine/engine.h"
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include "../testdata.h"

using namespace reno;

static int g_pass = 0, g_fail = 0;
static bool check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " - ", detail.c_str()); std::fflush(stdout); }
    return ok;
}

static std::unique_ptr<MediaReader> dcOpen(const std::string& path, std::string*);
class Dc : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t, int frames, int, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) l[k] = r[k] = 0.25f;
        return true;
    }
};
static std::unique_ptr<MediaReader> dcOpen(const std::string& path, std::string*)
{
    std::unique_ptr<Dc> d(new Dc());
    d->mi.path = path;
    d->mi.hasAudio = true;
    d->mi.duration = 60;
    d->mi.sampleRate = 48000;
    d->mi.channels = 2;
    return std::unique_ptr<MediaReader>(d.release());
}

static FxSlot efxSlot(Project& p, const char* name, uint32_t id)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "audio";
    s.name = name;
    s.audio.format = "VST2";
    s.audio.uid = strfmt("%u", id);
    s.audio.name = name;
    std::string err;
    if (auto pl = efxCreate(s.audio, &err)) s.state = pl->saveState();       // Reno's own copy of the effect, as it starts
    return s;
}

static Project made(FxSlot& limiter, FxSlot& reverb)
{
    Project p;
    p.settings.width = 1280;
    p.settings.height = 720;
    p.settings.fpsNum = 25;
    p.settings.fpsDen = 1;
    p.settings.sampleRate = 48000;
    MediaItem m;
    m.id = p.newId();
    m.path = "C:\\Reno test\\song.wav";
    m.name = "song.wav";
    m.info.path = m.path;
    m.info.hasAudio = true;
    m.info.duration = 60;
    p.media.push_back(m);
    Track a;
    a.id = p.newId();
    a.type = TrackType::Audio;
    a.name = "Music";
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.start = 0;
    e.length = 8;
    a.events.push_back(e);
    limiter = efxSlot(p, "eFX Limiter", 0x4d583241u);      // 'MX2A'
    reverb = efxSlot(p, "eFX Reverb", 0x4d583233u);
    a.fx = {limiter, reverb};
    Envelope env;
    env.kind = "fx";
    env.target = reverb.id;
    env.param = "Mix";
    env.points = {{0.0, 1.0, CurveLinear}, {2.0, 0.0, CurveSlow}, {4.0, 0.25, CurveLinear}};
    a.envelopes.push_back(env);
    p.tracks.push_back(a);
    return p;
}

// the points chunk of the first type 301 envelope in a written file
static bool plugEnvChunk(const std::string& bytes, std::string& out)
{
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    bool found = false;
    std::function<void(const veg::Chunk&)> walk = [&](const veg::Chunk& c) {
        for (const auto& k : c.kids) {
            if (k.isList) { walk(k); continue; }
            if (!found && k.id == veg::G::EnvelopePoints && k.data.u32(8) == veg::EnvPlugin) {
                out.assign((const char*)k.data.data(), k.data.size());
                found = true;
            }
        }
    };
    walk(t.root);
    return found;
}
static uint32_t u32at(const std::string& s, size_t o) { uint32_t v = 0; std::memcpy(&v, s.data() + o, 4); return v; }
static float f32at(const std::string& s, size_t o) { float v = 0; std::memcpy(&v, s.data() + o, 4); return v; }

int main()
{
    FxSlot lim, rev;
    Project p = made(lim, rev);
    std::vector<std::string> w;
    const std::string bytes = writeVegasData(p, "C:\\Reno test\\a.veg", &w);
    bool saidNotSaved = false;
    for (const auto& l : w) saidNotSaved = saidNotSaved || l.find("not saved") != std::string::npos;
    for (const auto& l : w) std::printf("   warning: %s\n", l.c_str());
    check(!bytes.empty() && !saidNotSaved, "the project with a plug-in envelope is saved with nothing left out");

    // 1. the record
    std::string rec;
    if (check(plugEnvChunk(bytes, rec), "the envelope is saved as type 301") && rec.size() >= 0x38 + 3 * 0x18) {
        check(u32at(rec, 0) == 0x38 && u32at(rec, 0x18) == 3 && u32at(rec, 0x34) == 0x18, "(header: points at 0x38, 3 points, 0x18 bytes each)");
        check(f32at(rec, 0x0c) == 0.f && f32at(rec, 0x10) == 100.f && f32at(rec, 0x14) == 50.f, "(lowest 0, highest 100, neutral 50 = Mix's default)");
        check(u32at(rec, 0x1c) == 9, "(setting 9: Mix)");
        check(u32at(rec, 0x20) == 1, "(second effect in the chain)");
        static const uint8_t clsid[16] = {0x33, 0x32, 0x58, 0x4d, 0xf1, 0xbd, 0xc5, 0xe7, 0x02, 0x69, 0x00, 0x07, 0xe9, 0x5d, 0x36, 0xfa};
        check(std::memcmp(rec.data() + 0x24, clsid, 16) == 0, "(the Reverb's CLSID)");
        check(f32at(rec, 0x38 + 8) == 100.f && f32at(rec, 0x38 + 0x18 + 8) == 0.f && f32at(rec, 0x38 + 0x30 + 8) == 25.f, "(values in percent)");
        check((int32_t)u32at(rec, 0x38 + 0x18 + 12) == -2 && u32at(rec, 0x38 + 12) == 1, "(curves: linear 1, slow -2)");
    }

    // read back
    Project q;
    const VegasImportResult r = importVegasData(bytes, "C:\\Reno test\\a.veg", q);
    check(r.ok, "the file reads back", r.error);
    for (const auto& n : r.notes) std::printf("   note: %s\n", n.c_str());
    if (!r.ok) return 1;
    uint64_t revId = 0;
    if (q.tracks.size() == 1 && q.tracks[0].fx.size() == 2) revId = q.tracks[0].fx[1].id;
    bool notUsed = false;
    for (const auto& n : r.notes) notUsed = notUsed || n.find("not used by Reno") != std::string::npos;
    check(!notUsed, "no note about an unused envelope");
    if (check(q.tracks.size() == 1 && q.tracks[0].envelopes.size() == 1, "one envelope on the track")) {
        const Envelope& e = q.tracks[0].envelopes[0];
        check(e.kind == "fx" && e.target == revId && e.param == "Mix", "it is the Reverb's Mix", e.kind + " " + e.param);
        check(e.points.size() == 3 && std::fabs(e.points[0].value - 1.0) < 1e-6 && std::fabs(e.points[1].value) < 1e-6 &&
                  std::fabs(e.points[2].value - 0.25) < 1e-6 && e.points[1].curve == CurveSlow && std::fabs(e.points[2].time - 4.0) < 1e-6,
              "points: percent turned into 0..1, times and curves kept");
    }

    // 2. saving
    {
        std::vector<std::string> w2;
        const std::string again = writeVegasData(q, "C:\\Reno test\\a.veg", &w2);
        check(again == bytes, "saving what was read changes no byte", strfmt("%zu vs %zu bytes", again.size(), bytes.size()));
        Project e1 = q;
        e1.tracks[0].envelopes[0].points[2].value = 0.75;
        std::string edited = writeVegasData(e1, "C:\\Reno test\\a.veg", nullptr), rec2;
        check(plugEnvChunk(edited, rec2) && rec2.size() == rec.size() && f32at(rec2, 0x38 + 0x30 + 8) == 75.f &&
                  std::memcmp(rec2.data() + 0x1c, rec.data() + 0x1c, 0x18) == 0,
              "an edited point is saved, the plug-in fields stay");
        Project e2 = q;
        std::swap(e2.tracks[0].fx[0], e2.tracks[0].fx[1]);
        std::string swapped = writeVegasData(e2, "C:\\Reno test\\a.veg", nullptr), rec3;
        check(plugEnvChunk(swapped, rec3) && u32at(rec3, 0x20) == 0 && u32at(rec3, 0x1c) == 9, "swapping the effects changes the place in the chain");
        Project e3 = q;
        e3.tracks[0].envelopes[0].target = 424242;
        std::vector<std::string> w3;
        std::string lost = writeVegasData(e3, "C:\\Reno test\\a.veg", &w3), rec4;
        bool said = false;
        for (const auto& l : w3) said = said || l.find("not saved") != std::string::npos;
        check(!plugEnvChunk(lost, rec4) && said, "an envelope for a plug-in the chain does not hold is not saved, and that is said");
        Project e4 = q;
        e4.tracks[0].envelopes[0].param = "7";
        std::string byNumber = writeVegasData(e4, "C:\\Reno test\\a.veg", nullptr), rec5;
        check(plugEnvChunk(byNumber, rec5) && u32at(rec5, 0x1c) == 7, "a setting can be named by its number");
    }

    // 3. a CLSID the chain does not hold: kept as it was
    {
        std::string odd = bytes;
        const char* tag = "\x33\x32\x58\x4d\xf1\xbd\xc5\xe7";
        const size_t at = rec.empty() ? std::string::npos : odd.find(rec);
        if (check(at != std::string::npos, "(the envelope record is in the file)")) {
            odd[at + 0x24 + 4] ^= 0x55;
            (void)tag;
            Project o;
            const VegasImportResult ro = importVegasData(odd, "C:\\Reno test\\a.veg", o);
            bool kept = false;
            for (const auto& n : ro.notes) kept = kept || n.find("not used by Reno") != std::string::npos;
            check(kept && o.tracks.size() == 1 && o.tracks[0].envelopes.empty(), "an unknown plug-in's envelope is reported and not used");
            check(writeVegasData(o, "C:\\Reno test\\a.veg", nullptr) == odd, "...and is written back byte for byte");
        }
    }

    // 4. playing it
    {
        FxRuntime fx;
        MediaPool pool;
        pool.setOpener(dcOpen);
        AudioEngine audio(pool, fx);
        fx.prepare(q);
        AudioPlugin* pl = revId ? fx.audio(revId) : nullptr;
        if (check(pl != nullptr && pl->paramCount() > 9, "the Reverb is made", revId ? fx.error(revId) : "none")) {
            std::vector<float> l(64), rr(64);
            auto mixAt = [&](double t) {
                audio.render(q, (int64_t)std::llround(t * 48000), 64, l.data(), rr.data(), true);
                return pl->paramGet(9);
            };
            check(std::fabs(mixAt(0.0) - 1.0f) < 0.01f, "at 0 s Mix is 1 (100 %)");
            check(std::fabs(mixAt(2.0) - 0.0f) < 0.01f, "at 2 s Mix is 0");
            check(std::fabs(mixAt(6.0) - 0.25f) < 0.01f, "after the last point Mix keeps its value (25 %)");
            const float mid = mixAt(1.0);
            check(mid > 0.05f && mid < 0.95f, "between the points it follows the curve", strfmt("%.3f", mid));
        }
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
