// Writing the envelopes and the effects (OpenFX, audio plug-in chains) of a .veg.
#include "vegwrite_internal.h"
#include "../audiofx/vegas_trkfx.h"
#include "../audiofx/vegaspack3.h"
#include "../audiofx/vegasfx_pack2.h"
#include "../audiofx/vegaspack1.h"

namespace reno {
namespace vegwrite {

// ---------------------------------------------------------------- envelopes
// VEGAS's envelope types (vegformat.h) for Reno's kinds; 0xFFFFFFFF: none.
uint32_t envelopeType(const Envelope& e, bool track, TrackType type, const std::map<uint64_t, uint32_t>& busIndex)
{
    const bool audio = type == TrackType::Audio;
    if (e.kind == "volume" && audio) return EnvVolume;
    if (e.kind == "pan" && audio) return EnvPan;
    if (e.kind == "composite" && !audio) return EnvComposite;
    if (e.kind == "mute" && track) return EnvMute;
    if (e.kind == "fadecolor" && track && !audio) return EnvFadeToColor;
    if (e.kind == "velocity" && !track && !audio) return EnvVelocity;
    if (e.kind == "send" && track && audio) {
        auto it = busIndex.find(e.target);
        if (it != busIndex.end() && it->second >= 1 && it->second <= 26) return EnvBusA + it->second - 1;
    }
    return 0xFFFFFFFFu;
}

// Lowest, highest and neutral value VEGAS gives an envelope type: from the samples for volume (0, 2, 1: up to +6 dB)
// and velocity (-1, 10, 1: -100% to 1000%); the others follow what the envelope does (VEGAS's ranges are not in
// any sample).
void envelopeRange(uint32_t type, float& lo, float& hi, float& neutral)
{
    switch (type) {
    case EnvVolume: lo = 0; hi = 2; neutral = 1; return;
    case EnvVelocity: lo = -1; hi = 10; neutral = 1; return;
    case EnvPan: lo = -1; hi = 1; neutral = 0; return;
    case EnvComposite: lo = 0; hi = 1; neutral = 1; return;
    case EnvFadeToColor: lo = -1; hi = 1; neutral = 0; return;
    case EnvMute: lo = 0; hi = 1; neutral = 0; return;
    default: break;
    }
    lo = 0; hi = 2; neutral = 1;        // sends: as volume
}

void Writer::envelopes(OutNode& list, const Envelopes& envs, const std::vector<VegHidden>* hidden, bool trackEnv, TrackType ttype, const Event* ev,
                       const Event* evRead, const std::string& where)
{
    const Chunk* tplList = nullptr;
    auto templateList = [&]() -> const Chunk* {
        if (!tplList) {
            tplList = sameFile([&](const Chunk& c) { return c.isList && c.type == (trackEnv ? G::TrackEnvelopes : G::EventEnvelopes) && c.kids.size() >= 2; });
            if (!tplList) tplList = tpl(trackEnv ? "trackEnvelopes" : "eventEnvelopes");
        }
        return tplList;
    };
    size_t h = 0;
    for (size_t i = 0; i <= envs.size(); ++i) {
        while (hidden && h < hidden->size() && (*hidden)[h].before <= i) {
            for (const Chunk* c : (*hidden)[h].chunks) list.kids.push_back(copyNode(*c));
            ++h;
        }
        if (i == envs.size()) break;
        const Envelope& e = envs[i];
        const uint32_t type = envelopeType(e, trackEnv, ttype, busIndex_);
        if (type == 0xFFFFFFFFu) {
            warn(where + ": its '" + e.kind + "' envelope was not saved (a VEGAS project cannot have one there, or how VEGAS keeps it is not known).");
            continue;
        }
        const auto* o = originOf<VegEnvelopeOrigin>(e.veg, VegOrigin::EnvelopeK);
        const Chunk* hc = o ? o->header : nullptr;
        const Chunk* pc = o ? o->chunk : nullptr;
        if (!pc || !hc) {
            const Chunk* l = templateList();
            if (!l || l->kids.size() < 2) { warn(where + ": an envelope could not be written (nothing to start from)."); continue; }
            hc = &l->kids[0];
            pc = &l->kids[1];
            o = nullptr;
        }
        // header: for an event's envelope, the event's start and length when the envelope last changed; they follow
        // the event where they were its own
        std::string hd = bytesOf(hc->data);
        if (ev) {
            if (!o) {
                setU64(hd, 0x08, (uint64_t)toTicks(ev->start));
                setU64(hd, 0x10, (uint64_t)toTicks(ev->length));
                setU64(hd, 0x30, (uint64_t)toTicks(ev->length));
            } else if (evRead) {
                if (hc->data.i64(0x08) == toTicks(evRead->start)) setU64(hd, 0x08, (uint64_t)toTicks(ev->start));
                if (hc->data.i64(0x10) == toTicks(evRead->length)) setU64(hd, 0x10, (uint64_t)toTicks(ev->length));
                if (hc->data.i64(0x30) == toTicks(evRead->length)) setU64(hd, 0x30, (uint64_t)toTicks(ev->length));
            }
        }
        std::string pd;
        if (o && samePoints(e, o->asRead) && type == o->vegType) {
            pd = bytesOf(pc->data);
        } else {
            const uint32_t first = pc->data.u32(0), step = pc->data.u32(0x34);
            pd = bytesOf(pc->data.sub(0, std::max<uint32_t>(0x38, first)));
            setU32(pd, 0x08, type);
            if (!o || type != o->vegType) {
                float lo, hi, neutral;
                envelopeRange(type, lo, hi, neutral);
                setF32(pd, 0x0c, lo);
                setF32(pd, 0x10, hi);
                setF32(pd, 0x14, neutral);
            }
            setU32(pd, 0x18, (uint32_t)e.points.size());
            const uint32_t st = step >= 16 ? step : 0x18;
            setU32(pd, 0x34, st);
            for (const auto& p : e.points) {
                std::string pt;
                addU64(pt, (uint64_t)toTicks(p.time));
                addF32(pt, (float)p.value);
                addU32(pt, (uint32_t)curveToVegas(e.kind == "mute" ? CurveHold : p.curve));
                pt.resize(st, '\0');
                pd += pt;
            }
        }
        list.kids.push_back(chunkNode(G::EnvelopeHeader, hd));
        list.kids.push_back(chunkNode(G::EnvelopePoints, pd));
    }
}

// ---------------------------------------------------------------- effects
std::string Writer::ofxRecord(const FxSlot& s, const std::string& where)
{
    const auto* o = originOf<VegFxOrigin>(s.veg, VegOrigin::FxK);
    if (o && !o->mediaFx && sameFxState(s, o->asRead)) return bytesOf(o->chunk->data.sub(o->entryOff, o->entryLen));
    // a managed effect (Rays, Glint) read from this file: written as it was, with the changed settings put in
    if (o && !o->mediaFx && isManagedFxAt(o->chunk->data, o->entryOff)) {
        ManagedFxRecord mr, nr;
        std::vector<std::string> w;
        if (readManagedFx(o->chunk->data, o->entryOff, mr) && s.pluginId == managedPluginId(mr) &&
            managedFxWith(mr, s.state, s.bypass, p_.settings.fps(), nr, w)) {
            for (const auto& line : w) warn(where + ": VEGAS " + mr.root + ": " + line + ".");
            std::string rec = writeManagedFx(o->chunk->data, o->entryOff, nr);
            setRecordBeforePanCrop(rec, s.beforePanCrop);        // bit 0x80 of the word at 0x2c, as for the older kind
            return rec;
        }
        o = nullptr;            // another effect in its place: written as a new OpenFX effect
    }
    // an older effect (Swirl, Pinch/Punch, Wave, TV Simulator): written as it was, with the changed settings put in
    if (o && !o->mediaFx && isLegacyFxAt(o->chunk->data, o->entryOff)) {
        LegacyFxRecord lr, nr;
        std::vector<std::string> w;
        if (readLegacyFx(o->chunk->data, o->entryOff, lr) && s.pluginId == legacyPluginId(lr) &&
            legacyFxWith(lr, s.state, p_.settings.fps(), nr, w)) {
            for (const auto& line : w) warn(where + ": VEGAS " + s.pluginId + ": " + line + ".");
            std::string rec = writeLegacyFx(o->chunk->data, o->entryOff, nr);
            setRecordBeforePanCrop(rec, s.beforePanCrop);
            setLegacyBypass(rec, s.bypass);
            return rec;
        }
        o = nullptr;
    }
    OfxWriteInput in;
    in.rawId = "{Svfx:" + s.pluginId + "}";
    in.state = s.state;
    if (o) {
        const Bytes orig = o->chunk->data.sub(o->entryOff, o->entryLen);
        in.original = orig;
        in.preset = orig.text(orig.u32(0) + orig.u32(0x28), orig.u32(0x2c));
        // switched off (0x01) and applied before pan/crop (0x80; a media effect keeps what it had)
        const uint32_t pre = o->mediaFx ? (orig.u32(0x20) & 0x80u) : (s.beforePanCrop ? 0x80u : 0u);
        in.flags = (orig.u32(0x20) & ~0x81u) | (s.bypass ? 1u : 0u) | pre;
        if (s.pluginId != o->asRead.pluginId) {
            // another plug-in in the same place: nothing of the old one is kept
            in.original = Bytes();
            in.headerTemplate = orig;
            in.preset = "(Default)";
        }
    } else {
        const Chunk* t = sameFile([](const Chunk& c) { return !c.isList && c.id == G::Fx && isOfxRecordAt(c.data, 0); });
        if (!t) t = tpl("fx");
        if (t) in.headerTemplate = t->data;
        in.preset = "(Default)";
        in.flags = 0x400 | (s.bypass ? 1u : 0u) | (s.beforePanCrop ? 0x80u : 0u);
        // parameter layout: from the record it was copied from (another project's), or another record of the same
        // effect in this one
        const auto* fo = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK);
        if (fo && fo->asRead.pluginId == s.pluginId && isOfxRecordAt(fo->chunk->data, fo->entryOff)) {
            in.layout = fo->chunk->data.sub(fo->entryOff, fo->entryLen);
            in.preset = in.layout.text(in.layout.u32(0) + in.layout.u32(0x28), in.layout.u32(0x2c));
        } else {
            const std::string rawId = "{Svfx:" + s.pluginId + "}";
            const Chunk* same = sameFile([&](const Chunk& c) {
                return !c.isList && c.id == G::Fx && isOfxRecordAt(c.data, 0) && c.data.text(0x30, c.data.u32(0x28)) == rawId;
            });
            if (same) in.layout = same->data;
        }
    }
    const OfxWriteResult r = writeOfxRecord(in);
    if (!r.dropped.empty()) {
        std::string names;
        for (size_t i = 0; i < r.dropped.size() && i < 4; ++i) names += (i ? ", " : "") + r.dropped[i];
        warn(where + ": " + strfmt("%zu", r.dropped.size()) + " setting(s) of '" + s.pluginId + "' were not saved (" + names +
             "): Reno does not know whether they are whole numbers; VEGAS will use the effect's defaults for them. Opening the effect "
             "in Reno once and saving again saves them.");
    }
    return r.record;
}

void Writer::videoFx(OutNode& list, const FxChain& chain, const std::vector<VegHidden>* hidden, const std::string& where, size_t* written)
{
    size_t h = 0, n = 0;
    for (size_t i = 0; i <= chain.size(); ++i) {
        while (hidden && h < hidden->size() && (*hidden)[h].before <= i) {
            for (const Chunk* c : (*hidden)[h].chunks) { list.kids.push_back(copyNode(*c)); ++n; }
            ++h;
        }
        if (i == chain.size()) break;
        const FxSlot& s = chain[i];
        // Reno's own effects (Color Grading panel) have no VEGAS record
        if (s.kind == "builtin") warn(where + ": Reno's own \"" + s.name + "\" effect was not saved (a VEGAS project cannot hold it).");
        if (s.kind != "ofx") continue;
        if (const auto* o = originOf<VegFxOrigin>(s.veg, VegOrigin::FxK))
            if (o->mediaFx) continue;           // media effects are written with the media
        list.kids.push_back(chunkNode(G::Fx, ofxRecord(s, where)));
        ++n;
    }
    if (written) *written = n;
}

// VEGAS's CLSID for an audio plug-in; empty if unknown.
static std::string audioClsid(const FxSlot& s, const Chunk* root, const VegProjectOrigin* po)
{
    if (s.audio.format == "DX" || s.audio.uid.compare(0, 6, "dshow:") == 0 || s.audio.uid.compare(0, 4, "dmo:") == 0) {
        const size_t b = s.audio.uid.find('{'), e = s.audio.uid.find('}');
        if (b == std::string::npos || e == std::string::npos || e < b + 37) return std::string();
        return guidBytes(toLower(s.audio.uid.substr(b + 1, e - b - 1)).c_str());
    }
    if (s.audio.format == "VST2") {
        const uint32_t id = (uint32_t)std::strtoul(s.audio.uid.c_str(), nullptr, 10);
        uint8_t want[4] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24)};
        // a plug-in this project already has
        if (po && root) {
            static const uint8_t cg[16] = {0x01, 0x86, 0x12, 0xb4, 0x2e, 0x70, 0xd1, 0x11, 0x82, 0x53, 0x00, 0x00, 0x92, 0x90, 0xdd, 0x44};
            std::string found;
            std::function<void(const Chunk&)> walk = [&](const Chunk& c) {
                for (const auto& k : c.kids) {
                    if (!found.empty()) return;
                    if (k.isList) { walk(k); continue; }
                    const size_t ci = k.data.find(cg, 16, 0);
                    if (ci == std::string::npos) continue;
                    AudioChainParts parts;
                    if (!splitAudioChain(k.data.sub(ci, k.data.size() - ci), parts)) continue;
                    for (const auto& en : parts.entries) {
                        const size_t at = ci + en.first;
                        if (k.data.has(at, 16) && std::memcmp(k.data.data() + at, want, 4) == 0 &&
                            std::memcmp(k.data.data() + at + 8, "\x02\x69\x00\x07\xe9\x5d\x36\xfa", 8) == 0)
                            found = bytesOf(k.data.sub(at, 16));
                    }
                }
            };
            walk(*root);
            if (!found.empty()) return found;
        }
        // one used in the sample projects
        const KnownVst* byName = nullptr;
        const KnownVst* byId = nullptr;
        for (const auto& k : knownVsts()) {
            if (k.vstId != id) continue;
            if (!byId) byId = &k;
            if (s.audio.name == k.name || s.name == k.name) byName = &k;
        }
        if (const KnownVst* k = byName ? byName : byId) return std::string((const char*)k->clsid, 16);
    }
    return std::string();
}

bool Writer::audioChain(const FxChain& chain, const Bytes* original, const char* tplName, std::string& out, size_t* written, const std::string& where)
{
    AudioChainParts parts;
    bool ok = original && splitAudioChain(*original, parts);
    if (!ok) {
        const Chunk* t = sameFile([](const Chunk& c) { return !c.isList && (c.id == G::EventAudioFx || c.id == G::TrackAudioFx); });
        if (!t) t = tpl(tplName);
        if (!t || !splitAudioChain(t->data, parts)) return false;
    }
    std::string entries;
    size_t n = 0;
    for (const auto& slot : chain) {
        if (slot.kind != "audio") continue;
        // Reno's reproductions of VEGAS's DirectX effects save as those effects (same settings)
        FxSlot asDx;
        // (builtinaudio.cpp's Distortion: VEGAS Distortion's settings and saved form, vegasdistortion.h)
        auto dxUidForBuiltin = [](const std::string& uid) {
            const std::string pack1 = vegpack1::dxForUid(uid);      // sfppack1's effects (vegaspack1.h)
            if (!pack1.empty()) return pack1;
            if (const std::string p2 = pack2DxForBuiltin(uid); !p2.empty()) return p2;     // sfppack2 / sfresfilter (vegasfx_pack2.h)
            const std::string trk = vegtrk::dxUidForBuiltin(uid);      // Volume, Pan, Track EQ... (vegas_trkfx.h)
            if (!trk.empty()) return trk;
            if (uid == "reno.distortion") return std::string("dshow:{39224540-6F92-11D0-AEBC-00A0C9053912}");
            const vegpack3::Entry* e = vegpack3::entryForBuiltin(uid);      // the other sfppack3 effects (vegaspack3.h)
            return e ? std::string(e->dxUid) : std::string();
        };
        const bool reproduced = slot.audio.format == "BUILTIN" && !dxUidForBuiltin(slot.audio.uid).empty();
        if (reproduced) {
            asDx = slot;
            asDx.audio.format = "DX";
            asDx.audio.uid = dxUidForBuiltin(slot.audio.uid);
            asDx.audio.name = "VEGAS " + slot.audio.name;
            if (const vegpack3::Entry* e = vegpack3::entryForBuiltin(slot.audio.uid)) asDx.audio.name = e->dxName;
            if (asDx.name == slot.audio.name) asDx.name = asDx.audio.name;
        }
        const FxSlot& s = reproduced ? asDx : slot;
        const auto* o = originOf<VegFxOrigin>(s.veg, VegOrigin::FxK);
        if (o && sameFxState(s, o->asRead) && s.name == o->asRead.name) {
            entries += bytesOf(o->chunk->data.sub(o->entryOff, o->entryLen));
            ++n;
            continue;
        }
        if (s.audio.format == "VST3") {
            warnCount("%d VST3 audio effect(s) were not saved: no VEGAS project shows how VEGAS keeps a VST3 plug-in.");
            continue;
        }
        if (s.audio.format == "BUILTIN") {
            // Reno's stand-ins for VEGAS's DirectX effects: VEGAS has no such plug-in
            warnCount("%d of Reno's built-in audio effects (Pitch Shift, Chorus...) were not saved: VEGAS has no such plug-ins - use VEGAS's own of the same name there.");
            continue;
        }
        AudioEntryInput in;
        in.name = s.audio.name.empty() ? s.name : s.audio.name;
        in.bypass = s.bypass;
        Bytes origEntry;
        if (o) {
            origEntry = o->chunk->data.sub(o->entryOff, o->entryLen);
            in.clsid16 = bytesOf(origEntry.sub(0, 16));
            if (s.name != o->asRead.name) in.name = s.name;
            else in.name = origEntry.text(24, origEntry.u32(20));
        }
        if (in.clsid16.size() != 16 || (o && s.audio.uid != o->asRead.audio.uid)) in.clsid16 = audioClsid(s, root_, po_);
        if (in.clsid16.size() != 16)
            if (const auto* fo = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK))
                if (fo->asRead.audio.uid == s.audio.uid && fo->entryLen >= 16)
                    in.clsid16 = bytesOf(fo->chunk->data.sub(fo->entryOff, 16));    // as named in the project it came from
        if (in.clsid16.size() != 16) {
            warn(where + ": audio effect '" + in.name + "' was not saved: VEGAS names VST plug-ins by a number worked out on the machine they are "
                 "installed on, and no VEGAS project here has used this one. Add it again in VEGAS.");
            continue;
        }
        const std::string st = s.state.isString() ? s.state.asString() : std::string();
        // original data for what Reno does not keep: the plug-in's data (VST: the program after VEGAS's u32) and the
        // u32s VEGAS keeps around it
        Bytes origData, origPayload;
        if (o && origEntry.size() >= 24) {
            const uint32_t nl = origEntry.u32(20);
            const size_t q = 24 + (size_t)nl + 16;
            const uint32_t dl = origEntry.u32(q);
            if (origEntry.has(q + 4, (size_t)dl + 4)) {
                origPayload = origEntry.sub(q + 4, dl);
                in.trailer = origEntry.u32(q + 4 + dl);
                if (dl >= 4) {
                    in.vstLead = origPayload.u32(0);
                    origData = origPayload.sub(4, dl - 4);
                }
            }
        }
        if (s.audio.format == "VST2") {
            in.vst = true;
            const uint32_t id = (uint32_t)std::strtoul(s.audio.uid.c_str(), nullptr, 10);
            std::string prog;
            if (!st.empty() && vstProgram(st, id, origData, prog)) in.data = prog;
            else if (origData.size()) in.data = bytesOf(origData);
            else {
                warn(where + ": VST effect '" + in.name + "' has no saved settings in Reno; it was not saved (VEGAS needs its program).");
                continue;
            }
        } else {
            in.vstLead = 0;
            if (st.size() > 2 && st.compare(0, 2, "S:") == 0) in.data = base64Decode(st.substr(2));
            else if (origPayload.size()) in.data = bytesOf(origPayload);
            else warnCount("%d DirectX audio effect(s) had no saved settings in Reno; VEGAS will start them with their defaults.");
        }
        entries += audioEntry(in);
        ++n;
    }
    std::string head = parts.head;
    setU32(head, parts.countOff, (uint32_t)n);
    if (!(original && ok)) {
        // a chain made from another's runs at the project's sound format (WAVEFORMATEX after the name and a u64:
        // format, channels, samples per second, bytes per second, block size, bits)
        const size_t wfx = 40 + (size_t)Bytes((const uint8_t*)head.data(), head.size()).u32(36) + 8;
        if (wfx + 18 <= head.size() && p_.settings.sampleRate > 0) {
            const Bytes h((const uint8_t*)head.data(), head.size());
            const uint16_t block = h.u16(wfx + 12);
            setU32(head, wfx + 4, (uint32_t)p_.settings.sampleRate);
            setU32(head, wfx + 8, (uint32_t)p_.settings.sampleRate * block);
        }
    }
    out = head + entries;
    if (written) *written = n;
    return true;
}

}  // namespace vegwrite
}  // namespace reno
