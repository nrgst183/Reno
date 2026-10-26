// vegdump: prints what the VEGAS importer reads from a .veg file (project properties, media, buses, tracks, events with
// times, effects with settings, markers, notes).
//
//   vegdump [--brief] [--json] [--tree] project.veg...
//     --brief   one line per project: counts of tracks, events, effects and notes
//     --json    the imported project as Reno writes it (Project::toJson)
//     --tree    the file's chunk tree (GUIDs and sizes) before anything else
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include <cstdio>
#include <cstring>

using namespace reno;

static void printTree(const veg::Chunk& c, int depth)
{
    for (const auto& k : c.kids) {
        std::printf("%*s%s %s  @%zu  %zu bytes\n", depth * 2, "", k.isList ? "list" : "    ", k.isList ? k.type.c_str() : k.id.c_str(),
                    k.fileOffset, k.data.size());
        if (k.isList) printTree(k, depth + 1);
    }
}

static std::string paramsText(const Json& state, size_t maxParams)
{
    std::string s;
    size_t n = 0;
    for (const auto& m : state.get("params").members()) {
        if (n++ >= maxParams) { s += " ..."; break; }
        s += " " + m.first + "=";
        const Json& e = m.second;
        if (e.has("s")) {
            std::string t = e.get("s").asString();
            if (t.size() > 24) t = t.substr(0, 24) + "..";
            s += "\"" + t + "\"";
        } else {
            const Json& v = e.get("v");
            for (size_t i = 0; i < v.size(); ++i) s += strfmt(i ? ",%g" : "%g", v.at(i).asDouble());
            if (e.has("k")) s += strfmt("[%zu keys, last at frame %g]", e.get("k").size(), e.get("k").at(e.get("k").size() - 1).at(0).asDouble());
        }
    }
    return s;
}

static void printChain(const FxChain& c, const char* indent)
{
    for (const auto& f : c) {
        if (f.kind == "ofx")
            std::printf("%sfx ofx %s%s%s\n", indent, f.pluginId.c_str(), f.bypass ? " BYPASSED" : "", paramsText(f.state, 12).c_str());
        else
            std::printf("%sfx audio %s '%s'%s uid %s state %zu chars\n", indent, f.audio.format.c_str(), f.name.c_str(), f.bypass ? " BYPASSED" : "",
                        f.audio.uid.c_str(), f.state.isString() ? f.state.asString().size() : (size_t)0);
    }
}

static int countFx(const Project& p)
{
    int n = 0;
    p.forEachFx([&](const FxSlot&) { ++n; });
    return n;
}

int main(int argc, char** argv)
{
    bool brief = false, json = false, tree = false;
    int status = 0;
    for (int a = 1; a < argc; ++a) {
        const std::string arg = argv[a];
        if (arg == "--brief") { brief = true; continue; }
        if (arg == "--json") { json = true; continue; }
        if (arg == "--tree") { tree = true; continue; }
        if (tree) {
            std::string raw;
            if (readFile(arg, raw)) {
                veg::ChunkTree t;
                veg::readChunkTree(veg::Bytes((const uint8_t*)raw.data(), raw.size()), t);
                std::printf("== chunk tree of %s (%zu chunks%s%s)\n", arg.c_str(), t.count, t.damaged ? ", damaged: " : "", t.problem.c_str());
                printTree(t.root, 1);
            }
        }
        Project p;
        const VegasImportResult r = importVegasProject(arg, p);
        if (!r.ok) {
            std::printf("%s: FAILED: %s\n", arg.c_str(), r.error.c_str());
            status = 1;
            continue;
        }
        size_t events = 0;
        for (const auto& t : p.tracks) events += t.events.size();
        if (brief) {
            std::printf("%-60s tracks %2zu events %5zu effects %4d media %3zu markers %2zu notes %3zu\n", pathFileName(arg).c_str(), p.tracks.size(), events,
                        countFx(p), p.media.size(), p.markers.size(), r.notes.size());
            continue;
        }
        if (json) {
            std::printf("%s\n", p.toJson().dump(1).c_str());
            continue;
        }
        const ProjectSettings& s = p.settings;
        std::printf("== %s\n", arg.c_str());
        std::printf("project %dx%d %d/%d fps (%.3f), %d Hz, pixel aspect %.3f, tempo %.1f %d/4, length %.3f s, master volume %.3f\n", s.width, s.height,
                    s.fpsNum, s.fpsDen, s.fps(), s.sampleRate, s.pixelAspect, s.tempo, s.beatsPerMeasure, p.length(), p.masterVolume);
        if (!s.title.empty() || !s.artist.empty()) std::printf("title '%s' artist '%s'\n", s.title.c_str(), s.artist.c_str());
        std::printf("media (%zu):\n", p.media.size());
        for (const auto& m : p.media) {
            std::printf("  #%llu %s%s'%s' %s video %d audio %d still %d %dx%d dur %.3f\n", (unsigned long long)m.id, m.generated ? "GENERATED " : "",
                        m.offline ? "OFFLINE " : "", m.name.c_str(), m.generated ? m.generator.pluginId.c_str() : m.path.c_str(), m.info.hasVideo,
                        m.info.hasAudio, m.info.isStill, m.info.width, m.info.height, m.info.duration);
            if (m.generated) std::printf("      generator%s\n", paramsText(m.generator.state, 30).c_str());
        }
        for (const auto& b : p.buses) {
            std::printf("bus #%llu '%s' level %.3f\n", (unsigned long long)b.id, b.name.c_str(), b.level);
            printChain(b.fx, "    ");
        }
        if (!p.audioOut.empty()) { std::printf("master bus effects:\n"); printChain(p.audioOut, "    "); }
        if (!p.videoOut.empty()) { std::printf("video output effects:\n"); printChain(p.videoOut, "    "); }
        int ti = 0;
        for (const auto& t : p.tracks) {
            std::printf("track %d %s '%s'%s%s level %.3f pan %.3f out %llu height %d composite %s, %zu events\n", ++ti,
                        t.type == TrackType::Video ? "VIDEO" : "AUDIO", t.name.c_str(), t.mute ? " MUTE" : "", t.solo ? " SOLO" : "", t.level, t.pan,
                        (unsigned long long)t.output, t.height, t.composite.c_str(), t.events.size());
            printChain(t.fx, "    ");
            for (const auto& e : t.events) {
                const MediaItem* m = p.findMedia(e.mediaId);
                std::printf("  ev %9.4f +%8.4f =%9.4f off %9.4f rate %.4f gain %.3f fade %.3f/%.3f%s%s%s%s%s pitch %+.2f grp %llu '%s'%s%s\n", e.start, e.length,
                            e.end(), e.offset, e.rate, e.gain, e.fadeIn, e.fadeOut, e.mute ? " MUTE" : "", e.loop ? "" : " NOLOOP", e.reverse ? " REV" : "",
                            (t.type == TrackType::Video && !e.keepAspect) ? " STRETCH" : (t.type == TrackType::Video && e.resample == "off") ? " NORESAMPLE" : "",
                            e.keepPitch ? " KEEPPITCH" : "", e.pitch, (unsigned long long)e.group, m ? m->name.c_str() : "?", e.name.empty() ? "" : " name:",
                            e.name.c_str());
                printChain(e.fx, "      ");
                for (const auto& k : e.panCrop)
                    std::printf("      pan/crop t %.3f centre %.4f,%.4f size %.4f x %.4f rot %.2f curve %d\n", k.time, k.cx, k.cy, k.w, k.h, k.rotation, k.curve);
            }
        }
        for (const auto& m : p.markers) std::printf("%s %.3f +%.3f '%s'\n", m.region ? "region" : "marker", m.time, m.length, m.name.c_str());
        std::printf("notes (%zu):\n", r.notes.size());
        for (const auto& n : r.notes) std::printf("  - %s\n", n.c_str());
    }
    return status;
}
