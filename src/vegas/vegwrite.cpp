// Writing VEGAS Pro projects (.veg).
//
// A project read from a .veg keeps the file's bytes and, on each object, the record VEGAS wrote and a copy of the
// object as read (vegorigin.h). Writing rebuilds the file from the project as it is now:
//   - an unchanged object is written as the bytes VEGAS wrote;
//   - a changed object is written as a copy of those bytes with the changed fields put in (only fields whose
//     meaning is known; see vegformat.h and vegas.cpp);
//   - a new object is made from a record VEGAS wrote: one of the same kind from the same file if there is one, else
//     a template from vegtemplates.cpp, with every field Reno knows put in and the rest left as in VEGAS's record;
//   - deleted objects are left out, and every count, size, id and cross-reference (media ids in takes, event ids in
//     groups, bus numbers in tracks, effect counts, the last event id...) is made consistent.
// A project that never came from a .veg is written into the template project (an empty VEGAS 18 project).
// What a .veg cannot hold, or Reno cannot write yet, is left out and reported in the warnings.
//
// This file: file layout, project properties, buses, media, groups, markers and the public entry points.
// vegwrite_tracks.cpp: tracks and events. vegwrite_fx.cpp: envelopes and effects. vegwrite_util.cpp: helpers.
// The Writer class is declared in vegwrite_internal.h.
#include "vegwrite_internal.h"

namespace reno {
namespace vegwrite {

std::vector<std::string> Writer::warnings() const
{
    std::vector<std::string> v = warnLines_;
    for (const auto& k : warnOrder_) v.push_back(strfmt(k.c_str(), warnCounts_.at(k)));
    return v;
}

uint64_t Writer::newEventId(TrackType type)
{
    uint64_t n = ++lastEventId_;
    return ((uint64_t)(type == TrackType::Video ? 3 : 2) << 60) | (n & 0x0FFFFFFFFFFFFFFFull);
}

void markGrouped(OutNode& top, const std::set<uint64_t>& grouped);

// Media records of the file that stay: those of media still in the project (and kinds Reno does not read).
void Writer::prepareMedia()
{
    if (!po_) return;
    for (const auto& r : po_->media) {
        if (!r.chunk) continue;
        const MediaItem* m = r.renoId ? p_.findMedia(r.renoId) : nullptr;
        if (m && !mine(m->veg)) m = nullptr;
        switch (r.kind) {
        case VegMediaRecord::Other:
            writtenRecords_.insert(r.vegId);
            break;
        case VegMediaRecord::Subclip:
            if (m) {
                writtenRecords_.insert(r.vegId);
                subclips_[m->id].push_back({r.vegId, r.subStart, r.subLen, r.subReversed});
            }
            break;
        case VegMediaRecord::File:
        case VegMediaRecord::Generated:
            if (m) {
                const auto* o = vegOriginAs<VegMediaOrigin>(m->veg, VegOrigin::MediaK);
                if (o && o->vegId == r.vegId) {
                    writtenRecords_.insert(r.vegId);
                    fileRecord_[m->id] = r.vegId;
                }
            }
            break;
        }
    }
}

// ---------------------------------------------------------------- the whole file
std::string Writer::run()
{
    po_ = vegOriginAs<VegProjectOrigin>(p_.veg, VegOrigin::ProjectK);
    root_ = po_ ? po_->chunk : tpl("project");
    if (!root_) return std::string();
    if (po_) {
        lastEventId_ = po_->lastEventId & 0x0FFFFFFFFFFFFFFFull;
        lastMediaId_ = po_->maxMediaId;
        lastTrackId_ = po_->maxTrackId;
    } else {
        // template project ids are not used: its media and tracks are not written
        lastEventId_ = 0;
        lastMediaId_ = 0;
        lastTrackId_ = 0;
    }
    // ids that stay: events and tracks written with their own records
    for (const auto& t : p_.tracks) {
        if (const auto* o = originOf<VegTrackOrigin>(t.veg, VegOrigin::TrackK)) lastTrackId_ = std::max(lastTrackId_, o->vegId);
        for (const auto& e : t.events)
            if (const auto* o = originOf<VegEventOrigin>(e.veg, VegOrigin::EventK))
                lastEventId_ = std::max<uint64_t>(lastEventId_, o->vegId & 0x0FFFFFFFFFFFFFFFull);
    }
    // bus numbers: Bus A is 1, in the order of the project's buses
    for (size_t i = 0; i < p_.buses.size(); ++i) busIndex_[p_.buses[i].id] = (uint32_t)i + 1;
    if (p_.buses.size() > 26) warn(strfmt("The project has %zu buses; a VEGAS project has at most 26 (Bus A to Bus Z), so the others were left out.", p_.buses.size()));
    {
        // Trimmer markers and regions: VEGAS writes them into the media files, which Reno leaves alone
        size_t marked = 0;
        for (const MediaItem& m : p_.media) marked += m.markers.empty() ? 0 : 1;
        if (marked)
            warn(strfmt("%zu media file(s) have Trimmer markers or regions; VEGAS stores those in the media file, which Reno does not "
                        "change, so they were not saved (Reno's own project files keep them).", marked));
    }

    {
        // image sequences (File > Import > Image Sequence): no sample shows how VEGAS stores them
        size_t seqs = 0, alpha = 0;
        for (const MediaItem& m : p_.media) {
            seqs += m.sequenceFps > 0 ? 1 : 0;
            alpha += m.alphaMode.empty() ? 0 : 1;
        }
        if (alpha)
            warn(strfmt("%zu media file(s) have an Alpha channel setting in Media Properties, which was not saved (Reno does not know where a .veg "
                        "stores it; Reno's own project files keep it).", alpha));
        if (seqs)
            warn(strfmt("%zu image sequence(s) were saved as their first picture (Reno does not know how a .veg stores image sequences; "
                        "Reno's own project files keep them).", seqs));
    }

    // tracks first (they decide which media records are needed), then the rest in VEGAS's order
    prepareMedia();
    std::vector<OutNode> tracks;
    for (size_t i = 0; i < p_.tracks.size(); ++i) tracks.push_back(track(p_.tracks[i], i));

    OutNode top;
    top.isList = true;
    bool groupsDone = false, markersDone = false;
    auto addGroups = [&](const Chunk* orig) {
        groupsDone = true;
        OutNode g = groups(orig);
        if (!g.kids.empty()) top.kids.push_back(std::move(g));
    };
    auto addMarkers = [&](const Chunk* orig) {
        markersDone = true;
        // marker lists in VEGAS's order: markers and regions, command markers, CD markers
        for (int which = 0; which < 3; ++which) {
            OutNode m = markers(orig, which);
            if (!m.kids.empty()) top.kids.push_back(std::move(m));
        }
    };
    const Chunk* origGroups = childList(*root_, G::Groups);
    const Chunk* origMarkers = childList(*root_, G::Markers);
    for (const auto& k : root_->kids) {
        if (!k.isList && k.id == G::Header) { top.kids.push_back(header()); continue; }
        if (k.isList && k.type == G::MediaList) {
            top.kids.push_back(OutNode());          // filled in below, once the tracks have said what they use
            continue;
        }
        if (k.isList && k.type == G::Groups) { if (po_ && !groupsDone) addGroups(&k); continue; }
        if (k.isList && k.type == G::Markers) { if (po_ && !markersDone) addMarkers(&k); continue; }
        if (k.isList && (k.type == G::CommandMarkers || k.type == G::CdMarkers)) {
            // written with the markers
            if (po_ && !markersDone) addMarkers(origMarkers);
            continue;
        }
        if (k.isList && k.type == G::Tracks) {
            // groups come before the tracks
            if (!groupsDone) addGroups(po_ ? origGroups : nullptr);
            OutNode tl = listNode(G::Tracks);
            for (auto& t : tracks) tl.kids.push_back(std::move(t));
            top.kids.push_back(std::move(tl));
            continue;
        }
        if (k.isList && k.type == G::Summary) {
            OutNode s = listNode(G::Summary);
            for (const auto& c : k.kids) s.kids.push_back((!c.isList && c.id == G::SummaryData) ? summary(c) : copyNode(c));
            top.kids.push_back(std::move(s));
            // markers come after the summary
            if (!markersDone) addMarkers(po_ ? origMarkers : nullptr);
            continue;
        }
        if (k.isList && k.type == G::Buses) { top.kids.push_back(buses(&k)); continue; }
        top.kids.push_back(copyNode(k));
    }
    if (!markersDone) addMarkers(nullptr);
    // the media list, in its place
    for (auto& k : top.kids)
        if (!k.isList && k.id.empty()) { k = media(); break; }

    // every event's "in a group" word, now that groups are known
    markGrouped(top, grouped_);
    // header's last event id, patched once everything has its id
    for (auto& k : top.kids)
        if (!k.isList && k.id == guidBytes(G::Header)) {
            if (!po_ || lastEventId_ != (po_->lastEventId & 0x0FFFFFFFFFFFFFFFull)) setU64(k.data, 0x48, lastEventId_);
            break;
        }
    return writeTree(top);
}

// ---------------------------------------------------------------- project properties
OutNode Writer::header()
{
    const Chunk* hc = childChunk(*root_, G::Header);
    std::string d = bytesOf(hc->data);
    const ProjectSettings& s = p_.settings;
    const ProjectSettings* r = po_ ? &po_->settings : nullptr;
    // texts: project path, documents folder, VEGAS settings folder, one more
    const Bytes b = hc->data;
    size_t at = b.u32(0);
    size_t textStart = at;
    std::vector<std::string> texts;
    for (int i = 0; i < 4; ++i) texts.push_back(b.textZ(at, &at));
    std::string rest = d.substr(std::min(at, d.size()));
    std::string fixed = d.substr(0, std::min(textStart, d.size()));
    // where the blocks of video settings are, relative to 'rest'
    const std::string projectPath = vegasPath(path_);
    bool textsChanged = false;
    if (!po_ || projectPath != po_->headerPath) {
        if (!projectPath.empty()) { texts[0] = projectPath; textsChanged = true; }
    }
    if (!po_) {
#ifdef _WIN32
        // new project: this machine's folders, as VEGAS would write them
        if (const char* up = std::getenv("USERPROFILE")) { texts[1] = std::string(up) + "\\Documents"; textsChanged = true; }
        if (const char* la = std::getenv("LOCALAPPDATA")) { texts[2] = std::string(la) + "\\VEGAS Pro\\18.0\\"; textsChanged = true; }
#endif
    }
    if (textsChanged) {
        std::string t;
        for (size_t i = 0; i < texts.size(); ++i) {
            const std::string u = utf16(texts[i], true);
            if (i < 3) setU32(fixed, 0x3c + 4 * i, (uint32_t)u.size());
            t += u;
        }
        d = fixed + t + rest;
        // drop the old padding (zeros after the third settings block) and pad again
        const size_t blocks = fixed.size() + t.size() + 3 * 0x358;
        if (d.size() > blocks) {
            bool zeros = true;
            for (size_t i = blocks; i < d.size(); ++i) zeros = zeros && d[i] == 0;
            if (zeros) d.resize(blocks);
        }
        pad8(d);
        at = fixed.size() + t.size();
    }
    const size_t blk = at;                           // the project's block of video settings
    if (!r || s.sampleRate != r->sampleRate) setU32(d, 0x0c, (uint32_t)s.sampleRate);
    if (!r || s.fpsNum != r->fpsNum || s.fpsDen != r->fpsDen) {
        setF64(d, 0x10, s.fps());
        setF64(d, blk + 0x38, s.fps());
    }
    if (!r || s.tempo != r->tempo) setF64(d, 0x18, s.tempo);
    if (!r || s.beatsPerMeasure != r->beatsPerMeasure) setU16(d, 0x28, (uint16_t)s.beatsPerMeasure);
    if (!r || s.width != r->width || s.height != r->height) {
        setU32(d, blk + 4, (uint32_t)s.width);
        setU32(d, blk + 8, (uint32_t)s.height);
    }
    if (!r || s.pixelAspect != r->pixelAspect) setF64(d, blk + 0x40, s.pixelAspect);
    // Project Properties whose place in the file is unknown
    const ProjectSettings def;
    const ProjectSettings& was = r ? *r : def;
    std::vector<std::string> lost;
    if (s.fieldOrder != was.fieldOrder) lost.push_back("field order");
    if (s.pixelFormat != was.pixelFormat) lost.push_back("pixel format");
    if (!r || s.renderQuality != r->renderQuality) {
        static const char* const names[] = {"", "draft", "preview", "good", "best"};
        for (uint32_t q = 1; q <= 4; ++q) if (s.renderQuality == names[q]) setU32(d, blk + 0x1c, q);
    }
    if (s.resample != was.resample) lost.push_back("resample mode");
    if (s.motionBlur != was.motionBlur) lost.push_back("motion blur type");
    if (!r || s.deinterlace != r->deinterlace) {
        if (s.deinterlace == "none") setU32(d, blk + 0xf4, 0);
        else if (s.deinterlace == "blend") setU32(d, blk + 0xf4, 1);
        else if (s.deinterlace == "interpolate") setU32(d, blk + 0xf4, 2);
    }
    if (s.rulerFormat != was.rulerFormat || s.rulerStart != was.rulerStart) lost.push_back("ruler");
    if (s.output360 != was.output360) lost.push_back("360\xC2\xB0 output");
    if (s.masterDisplay != was.masterDisplay) lost.push_back("master display");
    if (s.viewTransform != was.viewTransform || s.acesVersion != was.acesVersion) lost.push_back("view transform");
    if (!lost.empty()) {
        std::string l;
        for (size_t i = 0; i < lost.size(); ++i) l += (i ? ", " : "") + lost[i];
        warn("Project Properties: where VEGAS keeps the " + l + " is not known, so the change was not saved.");
    }
    return chunkNode(G::Header, d);
}

// INFO-style entries: u32 8, u32 count, then four-character code (big-endian), u32 bytes, UTF-16 text.
OutNode Writer::summary(const Chunk& c)
{
    const ProjectSettings& s = p_.settings;
    const ProjectSettings* r = po_ ? &po_->settings : nullptr;
    if (r && s.title == r->title && s.artist == r->artist && s.engineer == r->engineer && s.copyright == r->copyright && s.comments == r->comments)
        return copyNode(c);
    const Bytes& b = c.data;
    std::string d;
    addU32(d, b.u32(0));
    const uint32_t n = b.u32(4);
    addU32(d, n);
    size_t at = 8;
    for (uint32_t i = 0; i < n && i < 64 && b.has(at, 8); ++i) {
        const uint32_t cc = b.be32(at), len = b.u32(at + 4);
        if (!b.has(at + 8, len)) break;
        const std::string* text = nullptr;
        switch (cc) {
        case 0x494e414du: text = &s.title; break;
        case 0x49415254u: text = &s.artist; break;
        case 0x49454e47u: text = &s.engineer; break;
        case 0x49434f50u: text = &s.copyright; break;
        case 0x49434d54u: text = &s.comments; break;
        default: break;
        }
        d += bytesOf(b.sub(at, 4));
        const std::string u = text ? utf16(*text, true) : bytesOf(b.sub(at + 8, len));
        addU32(d, (uint32_t)u.size());
        d += u;
        at += 8 + (size_t)len;
    }
    return chunkNode(G::SummaryData, d);
}

// ---------------------------------------------------------------- buses
// The mixer chunk (sample rate at 0x6e4, bit depth at 0x6e8), the video bus, the master, then Bus A, B...
// A bus record: u32 0xc8, ..., u32 type at 0x0c, u32 number at 0x14, f64 level at 0x18 and 0x20, u32 at 0xcc the
// bytes from 0xd0 to the end (unpadded), three texts at 0xd0 (output device, name, name again), then (aligned to 4)
// 0x328 unknown bytes, u32 audio effect count, u32 chain bytes + 4, u32 0, the chain, u32 0, and 12 zero bytes.
std::string Writer::busRecord(const Bytes& orig, const FxChain* fx, const std::string* name, double level, uint32_t index, const std::string& where)
{
    std::string d = bytesOf(orig);
    if (orig.f64(0x18) != level) setF64(d, 0x18, level);
    if (orig.f64(0x20) != level) setF64(d, 0x20, level);
    if (index && orig.u32(0x14) != index) setU32(d, 0x14, index);
    // texts
    size_t at = 0xd0;
    std::vector<std::string> texts;
    for (int i = 0; i < 3; ++i) texts.push_back(orig.textZ(at, &at));
    const size_t aligned = (at + 3) & ~(size_t)3;
    const size_t chainHead = aligned + 0x328;
    if (!orig.has(chainHead, 12)) {
        if (fx || name) warn(where + ": its record is not laid out as expected; only its level was saved.");
        return d;
    }
    const uint32_t count = orig.u32(chainHead), clen = orig.u32(chainHead + 4);
    const std::string chainBytes = clen >= 4 && orig.has(chainHead + 12, clen) ? bytesOf(orig.sub(chainHead + 12, clen - 4)) : std::string();
    bool rebuild = false;
    if (name && (*name != texts[1] || *name != texts[2])) {
        texts[1] = *name;
        texts[2] = *name;
        rebuild = true;
    }
    std::string newChain = chainBytes;
    uint32_t newCount = count;
    if (fx) {
        const Bytes oc((const uint8_t*)chainBytes.data(), chainBytes.size());
        size_t written = 0;
        std::string c;
        if (audioChain(*fx, chainBytes.empty() ? nullptr : &oc, "eventAudioChain", c, &written, where)) {
            newChain = written ? c : std::string();
            newCount = (uint32_t)written;
            rebuild = true;
        }
    }
    if (!rebuild) return d;
    std::string out = d.substr(0, 0xd0);
    for (const auto& t : texts) out += utf16(t, true);
    while (out.size() % 4) out += '\0';
    out += bytesOf(orig.sub(aligned, 0x328));
    addU32(out, newCount);
    addU32(out, newChain.empty() ? 0u : (uint32_t)newChain.size() + 4);
    addU32(out, 0);
    if (!newChain.empty()) {
        out += newChain;
        addU32(out, 0);
    }
    out.append(12, '\0');
    setU32(out, 0xcc, (uint32_t)(out.size() - 0xd0));
    return out;
}

// Whether a chain of audio effects is the one in 'chunk' as VEGAS wrote it: same slots, unchanged, in order, none gone.
bool Writer::sameAudioChain(const FxChain& chain, const Chunk* chunk)
{
    size_t n = 0, prev = 0;
    for (const auto& f : chain) {
        if (f.kind != "audio") continue;
        const auto* fo = originOf<VegFxOrigin>(f.veg, VegOrigin::FxK);
        if (!fo || fo->chunk != chunk || !sameFxState(f, fo->asRead) || f.name != fo->asRead.name || (n > 0 && fo->entryOff <= prev)) return false;
        prev = fo->entryOff;
        ++n;
    }
    if (!chunk) return n == 0;
    static const uint8_t cg[16] = {0x01, 0x86, 0x12, 0xb4, 0x2e, 0x70, 0xd1, 0x11, 0x82, 0x53, 0x00, 0x00, 0x92, 0x90, 0xdd, 0x44};
    const size_t ci = chunk->data.find(cg, 16, 0);
    if (ci == std::string::npos) return n == 0;
    AudioChainParts parts;
    if (!splitAudioChain(chunk->data.sub(ci, chunk->data.size() - ci), parts)) return false;
    return parts.entries.size() == n;
}

OutNode Writer::buses(const Chunk* list)
{
    OutNode out = listNode(G::Buses);
    const Chunk* busTpl = nullptr;
    for (const auto& k : list->kids) {
        if (!k.isList && k.id == G::Mixer) {
            std::string d = bytesOf(k.data);
            const ProjectSettings& s = p_.settings;
            const ProjectSettings* r = po_ ? &po_->settings : nullptr;
            if (!r || s.sampleRate != r->sampleRate) setU32(d, 0x6e4, (uint32_t)s.sampleRate);
            if (!r || s.audioBits != r->audioBits) setU32(d, 0x6e8, (uint32_t)s.audioBits);
            out.kids.push_back(chunkNode(G::Mixer, d));
            continue;
        }
        if (k.isList || k.id != G::Bus) { out.kids.push_back(copyNode(k)); continue; }
        const uint32_t type = k.data.u32(0x0c);
        if (type == 3) continue;            // written after the master, below
        if (type == 7) {
            // video bus: no sample has effects on it, so they cannot be written
            bool same = true;
            for (const auto& f : p_.videoOut) {
                const auto* fo = originOf<VegFxOrigin>(f.veg, VegOrigin::FxK);
                if (!fo || fo->chunk != &k || !sameFxState(f, fo->asRead)) same = false;
            }
            if (!same) warn("The effects on the video output were not saved: no VEGAS project shows how VEGAS keeps them.");
            out.kids.push_back(copyNode(k));
            continue;
        }
        if (type != 2) { out.kids.push_back(copyNode(k)); continue; }
        // master: level and audio effects
        const bool levelSame = po_ && p_.masterVolume == po_->masterVolume;
        const bool fxSame = po_ && sameAudioChain(p_.audioOut, &k);
        if (levelSame && fxSame) out.kids.push_back(copyNode(k));
        else out.kids.push_back(chunkNode(G::Bus, busRecord(k.data, fxSame ? nullptr : &p_.audioOut, nullptr, p_.masterVolume, 0, "The master bus")));
        if (p_.masterPan != 0) warn("The master bus's pan was not saved: where VEGAS keeps it is not known.");
        if (p_.masterMute) warn("The master bus is muted in Reno; where VEGAS keeps that is not known, so it was not saved.");
        if (p_.masterFxPostFader) warn("The master's effects come after its volume (Post FX) in Reno; where VEGAS keeps that is not known, so it was not saved.");
        for (const Bus& bus : p_.buses) {
            if (bus.assignable) warn(bus.name + " is an Assignable FX chain in Reno; it was saved as a bus (how VEGAS keeps assignable FX is not known).");
            if (bus.fxPostFader) warn(bus.name + ": its effects come after its fader (Post Fader Insert FX) in Reno; where VEGAS keeps that is not known, so it was not saved.");
        }
        // the audio buses follow the master
        for (size_t i = 0; i < p_.buses.size() && i < 26; ++i) {
            const Bus& bus = p_.buses[i];
            const auto* o = originOf<VegBusOrigin>(bus.veg, VegOrigin::BusK);
            const std::string where = "Bus '" + bus.name + "'";
            const Chunk* base = o ? o->chunk : nullptr;
            if (!base) {
                if (!busTpl) busTpl = sameFile([](const Chunk& c) { return !c.isList && c.id == G::Bus && c.data.u32(0x0c) == 3; });
                if (!busTpl) busTpl = tpl("bus");
                base = busTpl;
            }
            if (!base) { warn(where + " could not be written (no bus record to start from)."); continue; }
            const bool chainSame = o && sameAudioChain(bus.fx, base);
            const uint32_t index = busIndex_[bus.id];
            const bool same = o && chainSame && bus.name == o->asRead.name && bus.level == o->asRead.level && index == base->data.u32(0x14);
            if (same) out.kids.push_back(copyNode(*base));
            else out.kids.push_back(chunkNode(G::Bus, busRecord(base->data, chainSame ? nullptr : &bus.fx, &bus.name, bus.level, index, where)));
            std::vector<std::string> lost;
            const Bus def;
            const Bus& was = o ? o->asRead : def;
            if (bus.pan != was.pan) lost.push_back("pan");
            if (bus.mute != was.mute) lost.push_back("mute");
            if (bus.solo != was.solo) lost.push_back("solo");
            if (bus.output != was.output) lost.push_back("output (VEGAS sends every bus to the master)");
            if (!bus.envelopes.empty()) lost.push_back("envelopes");
            if (!lost.empty()) {
                std::string l;
                for (size_t j = 0; j < lost.size(); ++j) l += (j ? ", " : "") + lost[j];
                warn(where + ": its " + l + " could not be saved: no VEGAS project shows where VEGAS keeps them.");
            }
        }
    }
    if (!p_.masterEnvelopes.empty()) warn("The master bus's envelopes were not saved: no VEGAS project shows how VEGAS keeps them.");
    if (!p_.videoEnvelopes.empty()) warn("The video output's envelopes were not saved: no VEGAS project shows how VEGAS keeps them.");
    if (p_.videoFadeTop != 0xFFFFFFFF || p_.videoFadeBottom != 0xFF000000)
        warn("The video output's fade colours were not saved: where VEGAS keeps them is not known.");
    return out;
}

// ---------------------------------------------------------------- media
// The path of media 'm' in the written file: as VEGAS wrote it if the item is unchanged (Reno may have found the
// file elsewhere, as VEGAS would), else where the item says.
std::string Writer::writtenPath(const MediaItem& m) const
{
    if (const auto* o = originOf<VegMediaOrigin>(m.veg, VegOrigin::MediaK))
        if (m.path == o->asRead.path)
            for (const auto& r : po_->media)
                if (r.renoId == m.id && (r.kind == VegMediaRecord::File || r.kind == VegMediaRecord::Subclip)) return r.path;
    return vegasPath(m.path);
}

// Id of the record naming the whole file of media 'm', created if the project has none.
uint32_t Writer::fileRecordFor(const MediaItem& m)
{
    auto it = fileRecord_.find(m.id);
    if (it != fileRecord_.end()) return it->second;
    const auto* o = originOf<VegMediaOrigin>(m.veg, VegOrigin::MediaK);
    if (o && o->vegId && o->chunk) {
        fileRecord_[m.id] = o->vegId;
        writtenRecords_.insert(o->vegId);
        return o->vegId;
    }
    // a new record, from one of the same kind
    const std::string ext = extOf(m.path);
    const bool veg = ext == ".veg";
    const bool still = isStill(m);
    const bool audioOnly = !veg && !still && m.info.hasAudio && !m.info.hasVideo;
    const uint32_t streams = still ? 1 : (audioOnly ? 1 : 2);
    const Chunk* base = nullptr;
    if (m.generated) {
        base = sameFile([](const Chunk& c) {
            if (c.isList || c.id != G::Media) return false;
            return c.data.text(0x80, c.data.u32(0x24)).compare(0, 22, "META:\\Video Generator\\") == 0;
        });
        if (!base) base = tpl("mediaGenerated");
    } else {
        base = sameFile([&](const Chunk& c) {
            if (c.isList || c.id != G::Media || c.data.u32(0) != 0x80) return false;
            const std::string pth = c.data.text(0x80, c.data.u32(0x24));
            return pth.compare(0, 5, "META:") != 0 && extOf(pth) == ext && c.data.u32(0x38) == streams && c.data.u32(0x48) == 0;
        });
        if (!base) base = tpl(veg ? "mediaVeg" : still ? "mediaVideo" : audioOnly ? "mediaAudio" : "mediaAV");
    }
    MediaParts parts;
    if (!base || !splitMedia(base->data, parts)) {
        warn("Media '" + m.name + "' could not be written (nothing to start from).");
        fileRecord_[m.id] = 0;
        return 0;
    }
    const uint32_t id = ++lastMediaId_;
    setU32(parts.head, 0x3c, id);
    if (m.generated) {
        // META:\Video Generator\{Svfx:<plug-in>} (<n>), generator name, frame size and its OpenFX record
        int n = 0;
        if (po_)
            for (const auto& r : po_->media) {
                const size_t open = r.path.rfind(" (");
                if (r.kind == VegMediaRecord::Generated || r.path.compare(0, 22, "META:\\Video Generator\\") == 0)
                    if (open != std::string::npos) n = std::max(n, std::atoi(r.path.c_str() + open + 2));
            }
        if (!po_) n = 0;
        n = std::max(n, generatorCounter_) + 1;
        generatorCounter_ = n;
        parts.path = utf16(strfmt("META:\\Video Generator\\{Svfx:%s} (%d)", m.generator.pluginId.c_str(), n), true);
        parts.name = utf16(m.name, true);
        // records after the name: 0x224 (+0x10 the same number, +0x1c the generator's name), 0xb0 (size), then u32 1
        // and the OpenFX record (anything after it is the media's own effects: none for a new one)
        std::string rest = parts.rest;
        size_t at = 0;
        std::string out;
        while (at + 8 <= rest.size()) {
            uint32_t sz = 0;
            std::memcpy(&sz, rest.data() + at, 4);
            if (sz == 0x224) {
                std::string r = rest.substr(at, sz);
                setU32(r, 0x10, (uint32_t)n);
                const std::string gn = utf16(m.generator.name.empty() ? m.name : m.generator.name, false);
                std::string field(0x224 - 0x1c, '\0');
                field.replace(0, std::min(field.size() - 2, gn.size()), gn.substr(0, std::min(field.size() - 2, gn.size())));
                r.replace(0x1c, field.size(), field);
                out += r;
            } else if (sz == 0xb0) {
                std::string r = rest.substr(at, sz);
                if (m.info.width > 0 && m.info.height > 0) {
                    setU32(r, 0x0c, (uint32_t)m.info.width);
                    setU32(r, 0x10, (uint32_t)m.info.height);
                    setU32(r, 0x14, (uint32_t)m.info.width);
                    setU32(r, 0x18, (uint32_t)m.info.height);
                }
                out += r;
            } else {
                break;
            }
            at += sz;
        }
        addU32(out, 1);
        out += ofxRecord(m.generator, "Generated media '" + m.name + "'");
        parts.rest = out;
    } else {
        parts.path = utf16(writtenPath(m), true);
        parts.name.clear();
        // one stream record (0x90 bytes, u32 type at +4: 3 video, 2 sound) per kind of stream
        std::string video, audio;
        for (size_t at = 0; at + 0x90 <= parts.rest.size(); at += 0x90) {
            uint32_t sz = 0, type = 0;
            std::memcpy(&sz, parts.rest.data() + at, 4);
            std::memcpy(&type, parts.rest.data() + at + 4, 4);
            if (sz != 0x90) break;
            if (type == 3 && video.empty()) video = parts.rest.substr(at, 0x90);
            if (type == 2 && audio.empty()) audio = parts.rest.substr(at, 0x90);
        }
        const bool wantVideo = still || veg || m.info.hasVideo || !m.info.hasAudio;
        const bool wantAudio = !still && (veg ? (m.info.hasAudio || !m.info.hasVideo) : m.info.hasAudio);
        if (video.empty() && !audio.empty()) { video = audio; setU32(video, 4, 3); }
        if (audio.empty() && !video.empty()) { audio = video; setU32(audio, 4, 2); }
        std::string streams = (wantVideo ? video : std::string()) + (wantAudio ? audio : std::string());
        if (!streams.empty()) {
            parts.rest = streams;
            setU32(parts.head, 0x38, (uint32_t)(streams.size() / 0x90));
        }
    }
    newMedia_.push_back(chunkNode(G::Media, joinMedia(parts)));
    newMedia_.back().tag = (int)id;
    fileRecord_[m.id] = id;
    writtenRecords_.insert(id);
    return id;
}

// A reversed subclip of 'm' that fits the range [needStart, needEnd) of the media: one the file has, or a new one
// from the start of the file to its end (or to where the event needs it, if the length is unknown).
uint32_t Writer::reversedSubclipFor(const MediaItem& m, double needStart, double needEnd, SubclipRef& out)
{
    auto it = subclips_.find(m.id);
    if (it != subclips_.end())
        for (const auto& s : it->second)
            if (s.reversed && s.start <= needStart + 1e-9 && s.start + s.len >= needEnd - 1e-9) { out = s; return s.vegId; }
    // the media is at least as long as the event needs
    const double len = std::max(m.info.duration > 0 ? m.info.duration : 0.0, needEnd);
    if (!(len > 0) || needStart < -1e-9) return 0;
    const Chunk* base = sameFile([](const Chunk& c) {
        return !c.isList && c.id == G::Media && c.data.text(0x80, c.data.u32(0x24)).compare(0, 14, "META:\\SubClip\\") == 0;
    });
    if (!base) base = tpl("mediaSubclip");
    MediaParts parts;
    if (!base || !splitMedia(base->data, parts)) return 0;
    const uint32_t id = ++lastMediaId_;
    setU32(parts.head, 0x3c, id);
    int n = 1;
    if (it != subclips_.end()) n = (int)it->second.size() + 1;
    parts.path = utf16(strfmt("META:\\SubClip\\{%s} (%d)[0][%lld][1]", writtenPath(m).c_str(), n, (long long)toTicks(len)), true);
    parts.name = utf16(fileNameOf(m.path) + strfmt(" - subclip %d (reversed)", n), true);
    // its streams: those the file has (one record per stream after the name)
    std::string streams;
    for (size_t at = 0; at + 0x90 <= parts.rest.size(); at += 0x90) {
        uint32_t sz = 0, type = 0;
        std::memcpy(&sz, parts.rest.data() + at, 4);
        std::memcpy(&type, parts.rest.data() + at + 4, 4);
        if (sz != 0x90) break;
        if ((type == 3 && m.info.hasVideo) || (type == 2 && m.info.hasAudio)) streams += parts.rest.substr(at, 0x90);
    }
    if (streams.empty()) return 0;
    setU32(parts.head, 0x38, (uint32_t)(streams.size() / 0x90));
    parts.rest = streams;
    newMedia_.push_back(chunkNode(G::Media, joinMedia(parts)));
    newMedia_.back().tag = (int)id;
    SubclipRef s{id, 0.0, len, true};
    subclips_[m.id].push_back(s);
    writtenRecords_.insert(id);
    out = s;
    return id;
}

OutNode Writer::media()
{
    OutNode list = listNode(G::MediaList);
    std::set<uint64_t> done;
    if (po_) {
        for (const auto& r : po_->media) {
            if (!r.chunk) continue;
            const MediaItem* m = r.renoId ? p_.findMedia(r.renoId) : nullptr;
            if (m && !mine(m->veg)) m = nullptr;
            switch (r.kind) {
            case VegMediaRecord::Other:
                list.kids.push_back(copyNode(*r.chunk));
                break;
            case VegMediaRecord::Subclip:
                if (m && writtenRecords_.count(r.vegId)) {
                    const auto* o = vegOriginAs<VegMediaOrigin>(m->veg, VegOrigin::MediaK);
                    if (o && m->path != o->asRead.path) {
                        // file found elsewhere: the subclip names the new place
                        MediaParts parts;
                        if (splitMedia(r.chunk->data, parts)) {
                            const std::string raw = r.chunk->data.text(0x80, r.chunk->data.u32(0x24));
                            const size_t close = raw.rfind("} (");
                            if (close != std::string::npos) {
                                parts.path = utf16("META:\\SubClip\\{" + vegasPath(m->path) + raw.substr(close), true);
                                list.kids.push_back(chunkNode(G::Media, joinMedia(parts)));
                                break;
                            }
                        }
                    }
                    list.kids.push_back(copyNode(*r.chunk));
                }
                break;
            case VegMediaRecord::File:
            case VegMediaRecord::Generated: {
                if (!m) break;
                const auto* o = vegOriginAs<VegMediaOrigin>(m->veg, VegOrigin::MediaK);
                if (!o || o->vegId != r.vegId) break;
                done.insert(m->id);
                mediaVegId[m->id] = r.vegId;
                const MediaItem& was = o->asRead;
                bool same = m->path == was.path && m->name == was.name;
                if (m->generated) same = same && sameFxState(m->generator, was.generator);
                if (same) { list.kids.push_back(copyNode(*r.chunk)); break; }
                MediaParts parts;
                if (!splitMedia(r.chunk->data, parts)) { list.kids.push_back(copyNode(*r.chunk)); break; }
                if (!m->generated && m->path != was.path) parts.path = utf16(vegasPath(m->path), true);
                if (m->name != was.name) parts.name = m->name.empty() ? std::string() : utf16(m->name, true);
                if (m->generated && !sameFxState(m->generator, was.generator)) {
                    const auto* fo = vegOriginAs<VegFxOrigin>(was.generator.veg, VegOrigin::FxK);
                    const auto* go = vegOriginAs<VegFxOrigin>(m->generator.veg, VegOrigin::FxK);
                    if (!go) go = fo;
                    if (go && go->chunk == r.chunk) {
                        // the generator's record is inside the rest, from entryOff (counted from the record's start)
                        const size_t restAt = r.chunk->data.size() - parts.rest.size();
                        if (go->entryOff >= restAt && go->entryOff + go->entryLen <= r.chunk->data.size()) {
                            FxSlot g = m->generator;
                            g.veg = was.generator.veg ? was.generator.veg : m->generator.veg;
                            const std::string rec = ofxRecord(g, "Generated media '" + m->name + "'");
                            parts.rest = parts.rest.substr(0, go->entryOff - restAt) + rec + parts.rest.substr(go->entryOff + go->entryLen - restAt);
                        }
                    }
                }
                list.kids.push_back(chunkNode(G::Media, joinMedia(parts)));
                break;
            }
            }
        }
    }
    // media new to this file (and items that had only subclips): a record of their own
    for (const auto& m : p_.media) {
        if (done.count(m.id)) continue;
        const auto* o = originOf<VegMediaOrigin>(m.veg, VegOrigin::MediaK);
        if (o && !o->vegId && !fileRecord_.count(m.id)) continue;   // only subclips are needed, and they are written
        const uint32_t id = fileRecordFor(m);
        if (id) mediaVegId[m.id] = id;
    }
    // in the project's media order: each item's record, then its subclips
    std::map<int, std::pair<size_t, size_t>> order;     // record id -> (item's place, place among its records)
    for (size_t i = 0; i < p_.media.size(); ++i) {
        auto f = fileRecord_.find(p_.media[i].id);
        if (f != fileRecord_.end()) order[(int)f->second] = {i, 0};
        auto sc = subclips_.find(p_.media[i].id);
        if (sc != subclips_.end())
            for (size_t k = 0; k < sc->second.size(); ++k) order.emplace((int)sc->second[k].vegId, std::make_pair(i, k + 1));
    }
    std::stable_sort(newMedia_.begin(), newMedia_.end(), [&](const OutNode& a, const OutNode& b) {
        const auto x = order.count(a.tag) ? order[a.tag] : std::make_pair((size_t)-1, (size_t)0);
        const auto y = order.count(b.tag) ? order[b.tag] : std::make_pair((size_t)-1, (size_t)0);
        return x < y;
    });
    for (auto& n : newMedia_) list.kids.push_back(std::move(n));
    newMedia_.clear();
    return list;
}

// ---------------------------------------------------------------- groups and markers
OutNode Writer::groups(const Chunk* orig)
{
    OutNode list = listNode(G::Groups);
    // Reno's groups by the VEGAS ids their events were written with
    std::map<uint64_t, std::vector<uint64_t>> groups;   // group -> event VEGAS ids, in track order
    std::map<uint64_t, uint64_t> groupOfVeg;            // event VEGAS id -> group
    for (const auto& t : p_.tracks)
        for (const auto& e : t.events)
            if (e.group) {
                auto it = eventVegId.find(e.id);
                if (it == eventVegId.end()) continue;
                groups[e.group].push_back(it->second);
                groupOfVeg[it->second] = e.group;
            }
    std::set<uint64_t> done;
    if (orig) {
        for (const auto& k : orig->kids) {
            if (k.isList || k.id != G::Group) { list.kids.push_back(copyNode(k)); continue; }
            const uint32_t n = k.data.u32(8);
            // kept as was if its events are still one group of exactly these events
            uint64_t g = 0;
            bool same = n >= 2;
            std::vector<uint64_t> ids;
            for (uint32_t i = 0; i < n && k.data.has(0x10 + (size_t)i * 8, 8); ++i) ids.push_back(k.data.u64(0x10 + (size_t)i * 8));
            for (uint64_t id : ids) {
                auto it = groupOfVeg.find(id);
                if (it == groupOfVeg.end()) { same = false; break; }
                if (g == 0) g = it->second;
                else if (g != it->second) { same = false; break; }
            }
            if (same && g && groups[g].size() == ids.size() && !done.count(g)) {
                list.kids.push_back(copyNode(k));
                done.insert(g);
                for (uint64_t id : ids) grouped_.insert(id);
            }
        }
    }
    const Chunk* tg = sameFile([](const Chunk& c) { return !c.isList && c.id == G::Group; });
    if (!tg) tg = tpl("group");
    for (const auto& g : groups) {
        if (done.count(g.first) || g.second.size() < 2) continue;
        std::string d;
        addU32(d, 0x10);
        addU32(d, tg ? tg->data.u32(4) : 1u);
        addU32(d, (uint32_t)g.second.size());
        addU32(d, 0);
        for (uint64_t id : g.second) {
            addU64(d, id);
            grouped_.insert(id);
        }
        list.kids.push_back(chunkNode(G::Group, d));
    }
    return list;
}

OutNode Writer::markers(const Chunk* orig, int which)
{
    OutNode list = listNode(which == 1 ? G::CommandMarkers : which == 2 ? G::CdMarkers : G::Markers);
    (void)orig;
    auto inList = [&](const Project::Marker& m) {
        if (which == 1) return m.kind == "command";
        if (which == 2) return m.kind == "cdtrack" || m.kind == "cdindex";
        return m.kind.empty();
    };
    bool any = false;
    for (const auto& m : p_.markers) {
        any = any || inList(m);
        if (which == 0 && !m.kind.empty() && m.kind != "command" && m.kind != "cdtrack" && m.kind != "cdindex")
            warnCount("%d marker(s) of a kind VEGAS does not have were not saved.");
    }
    if (!any) return list;
    // VEGAS numbers markers and regions separately; a region is a marker record with the region bit (0x01 at 0x1c)
    // and a length. All lists use the same records (readMarkers in vegas.cpp); a CD track is a region, a CD index a
    // marker, a command marker a marker whose second text is its command.
    uint32_t maxNum[2] = {0, 0};
    for (const auto& m : p_.markers)
        if (const auto* o = originOf<VegMarkerOrigin>(m.veg, VegOrigin::MarkerK)) {
            if (!inList(m) || !inList(o->asRead)) continue;
            const int k = (o->chunk->data.u32(0x1c) & 1) ? 1 : 0;
            maxNum[k] = std::max(maxNum[k], o->chunk->data.u32(0x18));
        }
    const Chunk* tm = sameFile([](const Chunk& c) { return !c.isList && c.id == G::Marker && c.data.u32(0) == 0x28; });
    if (!tm) tm = tpl("marker");
    for (const auto& m : p_.markers) {
        if (!inList(m)) continue;
        const auto* o0 = originOf<VegMarkerOrigin>(m.veg, VegOrigin::MarkerK);
        // a marker whose kind changed is a new record in another list; the old one is not reused
        const auto* o = (o0 && o0->asRead.kind == m.kind) ? o0 : nullptr;
        const Chunk* base = o ? o->chunk : tm;
        if (!base) continue;
        const bool region = which == 1 ? false : which == 2 ? m.kind == "cdtrack" : m.region;
        const Project::Marker def;
        const Project::Marker& was = o ? o->asRead : def;
        if (o && m.time == was.time && m.name == was.name && m.command == was.command && m.comment == was.comment && m.region == was.region &&
            (!m.region || m.length == was.length)) {
            list.kids.push_back(copyNode(*base));
            continue;
        }
        const Bytes& bd = base->data;
        const uint32_t textAt = bd.u32(0), nameBytes = bd.u32(0x20);
        std::string d = bytesOf(bd);
        // the two texts: the name (a command marker's comment, which Reno does not show, is kept) and the second
        // text: a command marker's command, otherwise kept as written
        std::string first = o ? d.substr(std::min<size_t>(textAt, d.size()), nameBytes) : utf16("", true);
        std::string second = o ? d.substr(std::min<size_t>((size_t)textAt + nameBytes, d.size()), bd.u32(0x24)) : std::string();
        if (which == 1) {
            if (!o || m.name != was.name || m.command != was.command) second = utf16(joinMarkerCommand(m.command, m.name), true);
            if (!o || m.comment != was.comment) first = utf16(m.comment, true);     // the comment
        } else if (!o || m.name != was.name) {
            first = utf16(m.name, true);
        }
        std::string after;
        // a CD track with an ISRC code has 16 more bytes after the texts: kept
        if (o && textAt == 0x30) after = d.substr(std::min<size_t>((size_t)textAt + nameBytes + bd.u32(0x24), d.size()));
        d = d.substr(0, textAt) + first + second;
        setU32(d, 0x20, (uint32_t)first.size());
        setU32(d, 0x24, (uint32_t)second.size());
        if (!after.empty()) {
            // the padding VEGAS added is not part of what follows the texts
            if (after.size() >= 16) after = after.substr(0, 16);
            d += after;
        }
        pad8(d);
        setU64(d, 0x08, (uint64_t)toTicks(m.time));
        setU64(d, 0x10, (uint64_t)toTicks(region ? std::max(0.0, m.length) : 0.0));
        const uint32_t flags = bd.u32(0x1c);
        const uint32_t kept = o ? flags : 0u;       // a new one: none of the template's
        const uint32_t nf = region ? (kept | 1u) : (kept & ~1u);
        if (nf != flags) setU32(d, 0x1c, nf);
        if (!o || region != was.region) setU32(d, 0x18, ++maxNum[region ? 1 : 0]);
        list.kids.push_back(chunkNode(G::Marker, d));
    }
    return list;
}

// After tracks and groups: every event's "in a group" word (0x48 of its record).
void markGrouped(OutNode& top, const std::set<uint64_t>& grouped)
{
    const std::string evData = guidBytes(G::EventData);
    std::function<void(OutNode&)> walk = [&](OutNode& n) {
        for (auto& k : n.kids) {
            if (k.isList) { walk(k); continue; }
            if (k.id != evData || k.data.size() < 0x4c) continue;
            uint64_t id = 0;
            std::memcpy(&id, k.data.data() + 0x40, 8);
            const uint32_t want = grouped.count(id) ? 1u : 0u;
            uint32_t have = 0;
            std::memcpy(&have, k.data.data() + 0x48, 4);
            if (have != want) setU32(k.data, 0x48, want);
        }
    };
    walk(top);
}

}  // namespace vegwrite

using namespace vegwrite;


// ------------------------------------------------------------------------------------------ public
namespace {
std::string writeInternal(const Project& p, const std::string& path, std::vector<std::string>* warnings, std::map<uint64_t, uint32_t>* mediaIds)
{
    Writer w(p, path);
    std::string bytes = w.run();
    if (warnings) *warnings = w.warnings();
    if (mediaIds) *mediaIds = w.mediaVegId;
    return bytes;
}
}  // namespace

std::string writeVegasData(const Project& p, const std::string& path, std::vector<std::string>* warnings)
{
    return writeInternal(p, path, warnings, nullptr);
}

namespace {

// Ties the project to the file just written: each object gets the origin of the object it became in 'q' (the file
// read back), so the next save starts from the file's content.
void adoptOrigins(Project& p, const Project& q, const std::map<uint64_t, uint32_t>& mediaIds)
{
    p.veg = q.veg;
    // media: by the id of the record written for it, else (items with only subclips) by path
    std::map<uint32_t, const MediaItem*> qByRecord;
    for (const auto& m : q.media)
        if (const auto* o = vegOriginAs<VegMediaOrigin>(m.veg, VegOrigin::MediaK))
            if (o->vegId) qByRecord[o->vegId] = &m;
    for (auto& m : p.media) {
        m.veg.reset();
        m.generator.veg.reset();
        const MediaItem* qm = nullptr;
        auto it = mediaIds.find(m.id);
        if (it != mediaIds.end()) {
            auto f = qByRecord.find(it->second);
            if (f != qByRecord.end()) qm = f->second;
        }
        if (!qm && !m.generated)
            for (const auto& x : q.media)
                if (!x.generated && toLower(x.path) == toLower(m.path)) { qm = &x; break; }
        if (!qm || qm->generated != m.generated) continue;
        // the item as it is now is what its record says (where Reno found the file is where the reader would)
        if (const auto* o = vegOriginAs<VegMediaOrigin>(qm->veg, VegOrigin::MediaK)) {
            auto c = std::make_shared<VegMediaOrigin>(*o);
            c->asRead = m;
            c->asRead.veg.reset();
            c->asRead.generator = qm->generator;
            m.veg = c;
        }
        if (m.generated) m.generator.veg = qm->generator.veg;
    }
    // tracks and events in order (the writer writes every track and event in the project's order)
    auto fxChain = [](FxChain& pc, const FxChain& qc) {
        // written slots in order: those that came back (VST3 and the like are left out by the writer)
        size_t j = 0;
        for (auto& s : pc) {
            s.veg.reset();
            while (j < qc.size()) {
                const auto* fo = vegOriginAs<VegFxOrigin>(qc[j].veg, VegOrigin::FxK);
                if (fo && fo->mediaFx) { ++j; continue; }
                break;
            }
            if (j < qc.size() && qc[j].kind == s.kind && qc[j].pluginId == s.pluginId && qc[j].audio.uid == s.audio.uid) {
                s.veg = qc[j].veg;
                ++j;
            }
        }
    };
    auto envs = [](Envelopes& pe, const Envelopes& qe) {
        size_t j = 0;
        for (auto& e : pe) {
            e.veg.reset();
            if (j < qe.size() && qe[j].kind == e.kind) { e.veg = qe[j].veg; ++j; }
        }
    };
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        Track& t = p.tracks[i];
        if (i >= q.tracks.size() || q.tracks[i].type != t.type) {
            t.veg.reset();
            for (auto& e : t.events) e.veg.reset();
            continue;
        }
        const Track& qt = q.tracks[i];
        t.veg = qt.veg;
        fxChain(t.fx, qt.fx);
        envs(t.envelopes, qt.envelopes);
        // events: by start, length and media (both are in time order)
        size_t j = 0;
        for (auto& e : t.events) {
            e.veg.reset();
            while (j < qt.events.size() && qt.events[j].start < e.start - 1e-6) ++j;
            if (j < qt.events.size() && std::fabs(qt.events[j].start - e.start) < 1e-6 && std::fabs(qt.events[j].length - e.length) < 1e-6) {
                e.veg = qt.events[j].veg;
                fxChain(e.fx, qt.events[j].fx);
                envs(e.envelopes, qt.events[j].envelopes);
                ++j;
            } else {
                for (auto& f : e.fx) f.veg.reset();
                for (auto& v : e.envelopes) v.veg.reset();
            }
        }
    }
    for (size_t i = 0; i < p.buses.size(); ++i) {
        if (i < q.buses.size()) {
            p.buses[i].veg = q.buses[i].veg;
            fxChain(p.buses[i].fx, q.buses[i].fx);
        } else {
            p.buses[i].veg.reset();
        }
    }
    fxChain(p.audioOut, q.audioOut);
    fxChain(p.videoOut, q.videoOut);
    for (size_t i = 0; i < p.markers.size(); ++i) p.markers[i].veg = i < q.markers.size() ? q.markers[i].veg : VegOriginPtr();
}

}  // namespace

VegasSaveResult saveVegasProject(Project& p, const std::string& path)
{
    VegasSaveResult r;
    std::vector<std::string> warnings;
    std::string bytes;
    std::map<uint64_t, uint32_t> mediaIds;
    try {
        bytes = writeInternal(p, path, &warnings, &mediaIds);
    } catch (const std::exception& ex) {
        r.error = std::string("the project could not be written (") + ex.what() + ")";
        return r;
    }
    if (bytes.empty()) {
        r.error = "the project could not be written (Reno has no VEGAS project to start from)";
        return r;
    }
    r.warnings = warnings;
    // write next to it, keep the old as .bak, then move the new one into place
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    const fs::path target(widen(path));
#else
    const fs::path target(path);
#endif
    fs::path tmp = target;
    tmp += ".reno-saving";
    fs::path bak = target;
    bak += ".bak";
    if (!writeFile(tmp.u8string(), bytes)) {
        r.error = "the file could not be written (is the folder writable, and is there room on the disk?)";
        fs::remove(tmp, ec);
        return r;
    }
    // what was written must read back as a project
    Project q;
    const VegasImportResult back = importVegasData(bytes, path, q);
    if (!back.ok) {
        fs::remove(tmp, ec);
        r.error = "the file written could not be read back (" + back.error + "); nothing was changed";
        return r;
    }
    bool moved = false;
    if (fs::exists(target, ec)) {
        fs::remove(bak, ec);
        ec.clear();
        fs::rename(target, bak, ec);
        moved = !ec;
        if (ec) {
            // could not move the old file aside: copy it
            ec.clear();
            fs::copy_file(target, bak, fs::copy_options::overwrite_existing, ec);
            ec.clear();
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        const std::string why = ec.message();
        // restore the old file
        if (moved) {
            std::error_code e2;
            fs::rename(bak, target, e2);
        }
        r.error = "the new file could not take the old one's place (" + why + "); it is at " + tmp.u8string();
        return r;
    }
    adoptOrigins(p, q, mediaIds);
    r.ok = true;
    return r;
}

}  // namespace reno
