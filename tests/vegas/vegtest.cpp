// vegtest: checks the VEGAS importer against real projects.
//
//   vegtest [folder with .veg files]       (default $RENO_SAMPLES, searched recursively)
//
// For every project:
//   - it opens, and nothing in it is reported damaged;
//   - the numbers of tracks, events, markers, OpenFX effects and VST effects match counts taken straight from the
//     file's bytes (the GUIDs that start those parts, the "{Svfx:" identifiers, the "CcnK" VST programs);
//   - every event lies at a sensible time and, where the media's length is known (VEGAS's subclips name it), plays
//     only media that exists; groups have at least two events;
//   - the project survives Project::toJson -> fromJson -> toJson unchanged;
//   - copies cut short at many places, and copies with bytes changed (at random, and in the chunks' size fields),
//     open or fail cleanly and quickly, without the address sanitizer finding anything.
// Plus facts that hold for every sample (see the checks near the end): normalising, bus routing, volume and velocity
// envelopes, generated media, picture sizes, markers, track motion.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/veglegacyfx.h"
#include "../../src/vegas/vegmanaged.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include "../testdata.h"

using namespace reno;

static int g_fail = 0, g_pass = 0;

static void check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
}

// The 16 bytes of a GUID as they are stored (the first three groups little-endian).
static std::string guidBytes(const char* g)
{
    unsigned a, b, c, d[8];
    std::sscanf(g, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    std::string s(16, '\0');
    for (int i = 0; i < 4; ++i) s[i] = (char)((a >> (8 * i)) & 0xff);
    for (int i = 0; i < 2; ++i) s[4 + i] = (char)((b >> (8 * i)) & 0xff);
    for (int i = 0; i < 2; ++i) s[6 + i] = (char)((c >> (8 * i)) & 0xff);
    for (int i = 0; i < 8; ++i) s[8 + i] = (char)d[i];
    return s;
}

static size_t countOf(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

// Occurrences of a GUID that start a chunk: followed by a 64-bit size that fits (a GUID can also appear
// inside a record, as a reference).
static size_t chunksOf(const std::string& hay, const std::string& guid)
{
    size_t n = 0;
    for (size_t at = hay.find(guid); at != std::string::npos; at = hay.find(guid, at + 1)) {
        if (at + 24 > hay.size()) continue;
        uint64_t size;
        std::memcpy(&size, hay.data() + at + 16, 8);
        n += (size >= 24 && size <= hay.size() - at) ? 1 : 0;
    }
    return n;
}

static std::string utf16(const std::string& ascii)
{
    std::string s;
    for (char c : ascii) { s += c; s += '\0'; }
    return s;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    size_t below(size_t n) { return n ? (size_t)(next() % n) : 0; }
};

// "... 4000x2250.jpg" -> 4000, 2250
static bool sizeInName(const std::string& path, int& w, int& h)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = toLower(path.substr(dot));
    if (ext != ".jpg" && ext != ".png") return false;
    size_t b = dot;
    while (b > 0 && std::isdigit((unsigned char)path[b - 1])) --b;
    if (b == dot || b == 0 || path[b - 1] != 'x') return false;
    size_t a = b - 1;
    while (a > 0 && std::isdigit((unsigned char)path[a - 1])) --a;
    if (a == b - 1) return false;
    w = std::atoi(path.substr(a, b - 1 - a).c_str());
    h = std::atoi(path.substr(b, dot - b).c_str());
    return w > 0 && h > 0;
}

static double g_slowest = 0;
static std::string g_slowestWhat;

// Opens damaged bytes; whatever happens must be a clean result.
static int g_damaged = 0;

static void openDamaged(const std::string& bytes, const std::string& what)
{
    ++g_damaged;
    Project p;
    const auto t0 = std::chrono::steady_clock::now();
    const VegasImportResult r = importVegasData(bytes, "/nonexistent/x.veg", p);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (secs > g_slowest) { g_slowest = secs; g_slowestWhat = what; }
    if (!r.ok && r.error.empty()) check(false, what + ": failed without saying why");
    if (r.ok) {
        // what was read must still be a sane project
        for (const auto& t : p.tracks)
            for (const auto& e : t.events)
                if (!(e.start >= 0 && e.length > 0 && e.rate > 0 && std::isfinite(e.offset))) {
                    check(false, what + ": an impossible event got through");
                    return;
                }
        // (every fifth: writing a big project out is slower than reading it)
        Project q;
        if (g_damaged % 5 == 0 && !q.fromJson(p.toJson())) check(false, what + ": the result does not survive JSON");
    }
}

static void walkOffsets(const veg::Chunk& c, std::vector<size_t>& out)
{
    for (const auto& k : c.kids) {
        out.push_back(k.fileOffset);
        if (k.isList) walkOffsets(k, out);
    }
}

static void fuzz(const std::string& raw, const std::string& name, uint64_t seed)
{
    Rng rng(seed);
    // How many damaged copies: more for small projects, fewer for big ones (each open of the biggest takes a
    // quarter of a second under the address sanitizer).
    const size_t budget = std::max<size_t>(36, std::min<size_t>(240, (size_t)(30e6 / std::max<size_t>(1, raw.size()))));
    // cut short: at the start, around the headers, at chunk boundaries (and just after them), and at
    // spread-out places
    std::set<size_t> cuts = {0, 1, 8, 16, 23, 24, 39, 40, 41, 63, 64};
    veg::ChunkTree tree;
    veg::readChunkTree(veg::Bytes((const uint8_t*)raw.data(), raw.size()), tree);
    std::vector<size_t> offs;
    walkOffsets(tree.root, offs);
    const size_t third = budget / 3;
    for (size_t i = 0; cuts.size() < third && i < offs.size(); i += std::max<size_t>(1, offs.size() / (third / 3 + 1))) {
        cuts.insert(offs[i]);
        cuts.insert(offs[i] + 1);
        cuts.insert(offs[i] + 40);
    }
    while (cuts.size() < third + 10) cuts.insert(rng.below(raw.size()));
    for (size_t at : cuts)
        if (at < raw.size()) openDamaged(raw.substr(0, at), name + strfmt(" cut at %zu", at));
    // changed bytes at random places
    for (size_t i = 0; i < third; ++i) {
        std::string d = raw;
        const int n = 1 + (int)rng.below(8);
        for (int k = 0; k < n; ++k) d[rng.below(d.size())] = (char)rng.below(256);
        openDamaged(d, name + strfmt(" random bytes #%zu", i));
    }
    // changed sizes, and changed fields at the starts of chunks (where the counts and lengths are)
    static const uint64_t sizes[] = {0, 1, 23, 24, 25, 0xffffffffull, 0xffffffffffffffffull, 0x7fffffffffffffffull, 0x100000000ull};
    for (size_t i = 0; i < third && !offs.empty(); ++i) {
        std::string d = raw;
        const size_t at = offs[rng.below(offs.size())];
        if (i % 2 == 0) {
            const uint64_t v = (i % 6 == 0) ? sizes[rng.below(9)] : rng.next() >> rng.below(64);
            if (at + 24 <= d.size()) std::memcpy(&d[at + 16], &v, 8);
        } else {
            const size_t field = at + 24 + 4 * rng.below(32);
            const uint32_t v = (uint32_t)((i % 3 == 0) ? 0xffffffffu : rng.next());
            if (field + 4 <= d.size()) std::memcpy(&d[field], &v, 4);
        }
        openDamaged(d, name + strfmt(" changed chunk #%zu", i));
    }
}

// A file made of lists nested far deeper than any project, and one claiming huge sizes: the reader must
// stop rather than recurse or allocate without end.
static void craftedFiles()
{
    const std::string riff = guidBytes(veg::G::Riff), list = guidBytes(veg::G::List), form = guidBytes(veg::G::VegForm);
    std::string deep = riff + std::string(8, '\0') + form;
    const int depth = 20000;
    const uint64_t total = 40 + (uint64_t)depth * 40;
    std::memcpy(&deep[16], &total, 8);
    for (int i = 0; i < depth; ++i) {
        std::string h = list + std::string(8, '\0') + guidBytes(veg::G::Tracks);
        const uint64_t sz = (uint64_t)(depth - i) * 40;
        std::memcpy(&h[16], &sz, 8);
        deep += h;
    }
    openDamaged(deep, "lists nested 20000 deep");
    std::string many = riff + std::string(8, '\0') + form;
    for (int i = 0; i < 200000; ++i) {
        std::string h = guidBytes(veg::G::Header) + std::string(8, '\0');
        const uint64_t sz = 24;
        std::memcpy(&h[16], &sz, 8);
        many += h;
    }
    const uint64_t msz = many.size();
    std::memcpy(&many[16], &msz, 8);
    openDamaged(many, "200000 empty chunks");
    openDamaged(std::string(), "an empty file");
    openDamaged("not a project at all", "a text file");
}

static size_t managedRecordsIn(const veg::Chunk& c)
{
    size_t n = 0;
    for (const auto& k : c.kids) {
        if (k.isList) { n += managedRecordsIn(k); continue; }
        veg::ManagedFxRecord r;
        veg::LegacyFxRecord l;
        if (k.id == veg::G::Fx && veg::readManagedFx(k.data, 0, r) && !veg::managedPluginId(r).empty()) ++n;
        else if (k.id == veg::G::Fx && veg::readLegacyFx(k.data, 0, l) && !veg::legacyPluginId(l).empty()) ++n;      // (veglegacy_test)
    }
    return n;
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    if (testDataMissing(dir)) return 0;
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir))
        if (e.is_regular_file() && endsWithNoCase(e.path().string(), ".veg")) files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    std::printf("%zu projects in %s\n", files.size(), dir.c_str());
    check(!files.empty(), "there are sample projects");

    const std::string gEvent = guidBytes(veg::G::EventData), gTrack = guidBytes(veg::G::Track), gMarker = guidBytes(veg::G::Marker);
    const std::string gGroup = guidBytes(veg::G::Group);
    const std::string svfx = utf16("{Svfx:"), ccnk = "CcnK";
    uint64_t seed = 1;
    for (const auto& f : files) {
        const std::string name = pathFileName(f);
        std::string raw;
        check(readFile(f, raw), name + ": readable");
        check(isVegasProject(f), name + ": recognised as a VEGAS project");
        Project p;
        const VegasImportResult r = importVegasProject(f, p);
        check(r.ok, name + ": opens (" + r.error + ")");
        if (!r.ok) continue;
        bool damaged = false;
        for (const auto& n : r.notes) damaged = damaged || n.find("damaged") != std::string::npos;
        check(!damaged, name + ": not reported damaged");

        size_t events = 0, ofx = 0, vst = 0, generated = 0;
        for (const auto& t : p.tracks) events += t.events.size();
        p.forEachFx([&](const FxSlot& s) {
            if (s.kind == "ofx") ++ofx;
            if (s.kind == "audio" && s.audio.format == "VST2") ++vst;
        });
        for (const auto& m : p.media) generated += m.generated ? 1 : 0;
        ofx -= generated;               // forEachFx counts the generators too
        // VEGAS's managed Rays and Glint (see vegmanaged_test) and its older Swirl, Pinch/Punch, Wave, TV Simulator and Light Rays
        // (veglegacy_test) are effects here but not OpenFX records in the file
        {
            veg::ChunkTree tree;
            veg::readChunkTree(veg::Bytes((const uint8_t*)raw.data(), raw.size()), tree);
            ofx -= std::min(ofx, managedRecordsIn(tree.root));
        }
        check(p.tracks.size() == countOf(raw, gTrack), name + strfmt(": %zu tracks, the file has %zu", p.tracks.size(), countOf(raw, gTrack)));  // (only ever a list type)
        check(events == chunksOf(raw, gEvent), name + strfmt(": %zu events, the file has %zu", events, chunksOf(raw, gEvent)));
        check(p.markers.size() == chunksOf(raw, gMarker), name + strfmt(": %zu markers, the file has %zu", p.markers.size(), chunksOf(raw, gMarker)));
        // a generated media entry names its generator twice (in its META path and in its record)
        check(ofx + 2 * generated == countOf(raw, svfx),
              name + strfmt(": %zu OpenFX effects + %zu generators, the file names %zu", ofx, generated, countOf(raw, svfx)));
        check(vst == countOf(raw, ccnk), name + strfmt(": %zu VST effects, the file has %zu VST programs", vst, countOf(raw, ccnk)));

        // events: sensible times, media that is there, offsets inside media whose length is known
        std::map<uint64_t, size_t> groups;
        int badTimes = 0, badMedia = 0, pastEnd = 0, unsorted = 0;
        for (const auto& t : p.tracks) {
            double prev = -1;
            for (const auto& e : t.events) {
                if (!(e.start >= 0 && e.length > 0 && e.rate > 0.001 && e.rate < 1000 && e.offset >= 0 && std::isfinite(e.gain))) ++badTimes;
                if (e.start < prev) ++unsorted;
                prev = e.start;
                const MediaItem* m = p.findMedia(e.mediaId);
                if (!m) { ++badMedia; continue; }
                if (m->info.duration > 0 && !m->info.isStill && e.offset > m->info.duration + 0.01) ++pastEnd;
                if (e.group) ++groups[e.group];
            }
        }
        check(badTimes == 0, name + strfmt(": %d events with impossible times", badTimes));
        check(unsorted == 0, name + ": events in order");
        check(badMedia == 0, name + strfmt(": %d events without media", badMedia));
        check(pastEnd == 0, name + strfmt(": %d events start past the end of their media", pastEnd));
        int lonely = 0;
        for (const auto& g : groups) lonely += g.second < 2 ? 1 : 0;
        check(lonely == 0, name + ": every group has two or more events");
        check(groups.size() <= chunksOf(raw, gGroup), name + ": no more groups than the file has");

        // round trip
        const std::string j1 = p.toJson().dump(1);
        Project q;
        std::string err;
        check(q.fromJson(p.toJson(), &err), name + ": loads back from JSON");
        check(q.toJson().dump(1) == j1, name + ": JSON round trip is exact");

        fuzz(raw, name, seed++);
    }
    craftedFiles();

    // ---------------------------------------------------------------- facts that hold for every sample
    // (worked out by hand once, here as invariants over all the projects: normalising, buses, envelopes, generated media,
    // picture sizes, markers, velocity envelopes and track motion)
    {
        int normalised = 0, badNormalise = 0, badBus = 0, badEnv = 0, envs = 0, titles = 0, badTitles = 0, badMarkers = 0;
        int named = 0, rightSize = 0, envNotes = 0, velChecked = 0, velRight = 0;
        bool motionTried = false;
        for (const auto& f : files) {
            Project p;
            const VegasImportResult r = importVegasProject(f, p);
            if (!r.ok) continue;
            for (const auto& n : r.notes) envNotes += n.find("volume envelope") != std::string::npos ? 1 : 0;
            // sound normalised (flag set) by its stored gain, kept apart from the event's own level as VEGAS keeps it
            std::set<uint64_t> busIds;
            for (const auto& b : p.buses) busIds.insert(b.id);
            for (const auto& t : p.tracks) {
                if (t.output != 0 && !busIds.count(t.output)) ++badBus;
                for (const auto& e : t.events)
                    if (e.normalize) {
                        ++normalised;
                        if (!(e.normalizeGain > 0 && std::isfinite(e.normalizeGain) && std::isfinite(e.gain * e.normalizeGain))) ++badNormalise;
                    }
                for (const auto& env : t.envelopes)
                    if (env.kind == "volume" && t.type == TrackType::Audio) {
                        ++envs;
                        for (const auto& pt : env.points) if (!(pt.value >= 0 && pt.value <= 4.0)) ++badEnv;     // gain factors: up to +12 dB
                    }
            }
            for (const auto& m : p.markers) if (!(m.time >= 0)) ++badMarkers;
            for (const auto& m : p.media) {
                if (m.generated && m.generator.pluginId == "com.vegascreativesoftware:titlesandtext") {
                    ++titles;
                    const std::string text = m.generator.state.get("params").get("Text").get("s").asString();
                    if (text.compare(0, 5, "{\\rtf") != 0 || m.info.width <= 0 || m.info.height <= 0) ++badTitles;
                }
                // pictures whose names give their sizes: the importer reads them from the pan/crop data
                int w = 0, h = 0;
                if (!sizeInName(m.path, w, h) || m.info.width == 0) continue;
                ++named;
                rightSize += (w == m.info.width && h == m.info.height) ? 1 : 0;
            }
            std::string raw;
            if (!readFile(f, raw)) continue;
            veg::ChunkTree tree;
            veg::readChunkTree(veg::Bytes((const uint8_t*)raw.data(), raw.size()), tree);
            // track motion: a project whose track motion is VEGAS's untouched default says nothing about it; the same project with
            // the motion key's width changed must say its track motion was left out
            if (!motionTried) {
                size_t at = 0;
                std::function<void(const veg::Chunk&)> find = [&](const veg::Chunk& c) {
                    for (const auto& k : c.kids) {
                        if (!k.isList && k.id == veg::G::TrackMotion) at = k.fileOffset + 24;
                        if (k.isList) find(k);
                    }
                };
                find(tree.root);
                if (at != 0 && at + 0x24 + 8 + 0x4c + 8 <= raw.size()) {
                    motionTried = true;
                    std::string changed = raw;
                    const double width = 1.5;                  // the motion key's width (frame heights) at +0x4c of the key
                    std::memcpy(&changed[at + 0x24 + 8 + 0x4c], &width, 8);
                    Project q;
                    const VegasImportResult r2 = importVegasData(changed, f, q);
                    int motionNotes = 0;
                    for (const auto& n : r2.notes) motionNotes += n.find("track motion") != std::string::npos ? 1 : 0;
                    check(r2.ok && motionNotes == 1, pathFileName(f) + " with its track motion changed: reported");
                }
            }
            // velocity envelopes are brought over as they are, and the event keeps its own rate
            std::function<void(const veg::Chunk&)> walk = [&](const veg::Chunk& c) {
                for (const auto& k : c.kids) {
                    if (k.isList && k.type == veg::G::Event) {
                        const veg::Chunk* d = k.child(veg::G::EventData);
                        const veg::Chunk* env = k.child(veg::G::EventEnvelopes);
                        if (!d || !env) continue;
                        for (const veg::Chunk* pts : env->children(veg::G::EnvelopePoints)) {
                            veg::EnvelopeRecord er;
                            if (!veg::readEnvelope(pts->data, er) || er.type != veg::EnvVelocity || er.points.empty()) continue;
                            bool flat = true;
                            for (const auto& pt : er.points) flat = flat && pt.value == er.points[0].value;
                            if (!flat) continue;
                            const double start = (double)d->data.i64(0x10) / 1e7, length = (double)d->data.i64(0x18) / 1e7;
                            const double rate = d->data.f64(0x28);
                            for (const auto& t : p.tracks)
                                for (const auto& e : t.events)
                                    if (t.type == TrackType::Video && std::fabs(e.start - start) < 1e-9 && std::fabs(e.length - length) < 1e-9) {
                                        ++velChecked;
                                        const Envelope* v = findEnvelope(e.envelopes, "velocity");
                                        velRight += (std::fabs(e.rate - rate) < 1e-12 && v && v->points.size() == er.points.size() &&
                                                     std::fabs(v->points[0].value - er.points[0].value) < 1e-6)
                                                        ? 1
                                                        : 0;
                                    }
                        }
                    } else if (k.isList) {
                        walk(k);
                    }
                }
            };
            walk(tree.root);
        }
        check(badNormalise == 0, strfmt("samples: %d normalised sounds all keep a usable normalise gain", normalised));
        check(badBus == 0, "samples: every track's output is the master or a bus of the project");
        check(badEnv == 0, strfmt("samples: track volume envelope values are gain factors (%d envelopes)", envs));
        check(badTitles == 0, strfmt("samples: %d Titles & Text media all have RTF text and a size", titles));
        check(badMarkers == 0, "samples: markers lie at or after 0");
        check(rightSize == named, strfmt("samples: picture sizes match their names (%d of %d)", rightSize, named));
        check(envNotes == 0, "samples: volume envelopes are not reported as unused");
        check(velRight == velChecked, strfmt("samples: velocity envelopes read as envelopes, rates kept (%d of %d)", velRight, velChecked));
    }

    std::printf("%d damaged copies opened\n", g_damaged);
    std::printf("slowest damaged copy: %.3f s (%s)\n", g_slowest, g_slowestWhat.c_str());
    check(g_slowest < 5.0, "every damaged copy opens or fails within 5 s");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
