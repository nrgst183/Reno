// Writing the tracks and events of a .veg: their records, takes, pan/crop and per-event settings.
#include "vegwrite_internal.h"

namespace reno {
namespace vegwrite {

// ---------------------------------------------------------------- tracks
OutNode Writer::track(const Track& t, size_t index)
{
    const auto* o = originOf<VegTrackOrigin>(t.veg, VegOrigin::TrackK);
    const bool video = t.type == TrackType::Video;
    const std::string label = t.name.empty() ? strfmt("Track %zu", index + 1) : strfmt("Track %zu ('%s')", index + 1, t.name.c_str());
    const Chunk* base = o ? o->chunk : nullptr;
    if (!base) {
        base = sameFile([&](const Chunk& c) {
            if (!c.isList || c.type != G::Track) return false;
            const Chunk* h = childChunk(c, G::TrackHeader);
            return h && h->data.u32(0x0c) == (video ? 3u : 2u);
        });
        if (!base) base = tpl(video ? "videoTrack" : "audioTrack");
    }
    const Track def;
    const Track& was = o ? o->asRead : def;
    OutNode out = listNode(G::Track);

    // the header
    uint32_t vegId = o ? o->vegId : 0;
    if (!vegId || usedTrackIds_.count(vegId)) vegId = ++lastTrackId_;
    usedTrackIds_.insert(vegId);
    out.kids.push_back(chunkNode(G::TrackHeader, trackHeader(t, index, *base, o, was, vegId, video)));

    trackEvents(t, o, out);

    // envelopes
    if (!t.envelopes.empty() || (o && !o->hiddenEnvelopes.empty())) {
        OutNode el = listNode(G::TrackEnvelopes);
        envelopes(el, t.envelopes, o ? &o->hiddenEnvelopes : nullptr, true, t.type, nullptr, nullptr, label);
        if (!el.kids.empty()) out.kids.push_back(std::move(el));
    }

    // the video or audio settings, and the effects
    FxChain videoSlots, audioSlots;
    for (const auto& f : t.fx) (f.kind == "audio" ? audioSlots : videoSlots).push_back(f);
    if (video && !audioSlots.empty()) warn(label + ": audio effects on a video track were not saved.");
    if (!video && !videoSlots.empty()) warn(label + ": video effects on an audio track were not saved.");
    size_t fxWritten = 0;
    OutNode fxList = listNode(G::TrackFx);
    bool haveFxList = false;
    std::string chain;
    size_t chainWritten = 0;
    bool haveChain = false;
    if (video) {
        const Chunk* origList = childList(*base, G::TrackFx);
        if (!videoSlots.empty() || (o && !o->hiddenFx.empty())) {
            videoFx(fxList, videoSlots, o ? &o->hiddenFx : nullptr, label, &fxWritten);
            haveFxList = true;
        } else if (o && origList) {
            haveFxList = true;          // VEGAS keeps an empty list once a track has had effects
        }
    } else {
        const Chunk* origList = childList(*base, G::TrackAudioFx);
        const Chunk* origChain = o && origList ? childChunk(*origList, G::TrackAudioFx) : nullptr;
        const bool same = o && sameAudioChain(audioSlots, origChain);
        if (same) {
            if (origChain) {
                chain = bytesOf(origChain->data);
                haveChain = true;
                chainWritten = audioSlots.size();
            }
        } else if (!audioSlots.empty()) {
            std::string c;
            const Bytes* ob = origChain ? &origChain->data : nullptr;
            if (audioChain(audioSlots, ob, "trackAudioChain", c, &chainWritten, label) && chainWritten > 0) {
                addU32(c, 0);
                chain = c;
                haveChain = true;
            }
        }
    }
    trackSettings(t, *base, o, was, video, label, fxWritten, chainWritten, out);
    if (video && haveFxList) out.kids.push_back(std::move(fxList));
    if (!video && haveChain) {
        OutNode l = listNode(G::TrackAudioFx);
        l.kids.push_back(chunkNode(G::TrackAudioFx, chain));
        out.kids.push_back(std::move(l));
    }
    // the rest of the original track (track motion, lists VEGAS keeps empty, anything unknown)
    if (o) {
        for (const auto& k : base->kids) {
            if (!k.isList && k.id == G::TrackHeader) continue;
            if (k.isList && (k.type == G::Events || k.type == G::TrackEnvelopes || k.type == G::VideoTrackList || k.type == G::AudioTrackList ||
                             k.type == G::TrackFx || k.type == G::TrackAudioFx))
                continue;
            out.kids.push_back(copyNode(k));
        }
    }
    trackWarnings(t, o, was, label);
    return out;
}

// The track header record: the original with whatever changed since it was read.
std::string Writer::trackHeader(const Track& t, size_t index, const Chunk& base, const VegTrackOrigin* o, const Track& was, uint32_t vegId, bool video)
{
    const Chunk* hc = childChunk(base, G::TrackHeader);
    std::string h = bytesOf(hc->data);
    if (!o || t.name != was.name) {
        const std::string n = t.name.empty() ? std::string() : utf16(t.name, true);
        h = h.substr(0, std::min<size_t>(h.size(), hc->data.u32(0))) + n;
        setU32(h, 0x10, (uint32_t)n.size());
        pad8(h);
    }
    // flags (TrackFlag in vegformat.h); unknown ones are kept
    const uint32_t f0 = hc->data.u32(4);
    uint32_t f = f0;
    if (!o) f &= ~(uint32_t)(TrackSelected | TrackArmed | TrackAutomation);     // new track: not selected, armed or automated
    if (!o || t.mute != was.mute) f = (f & ~(uint32_t)TrackMute) | (t.mute ? TrackMute : 0u);
    if (!o || t.solo != was.solo) f = (f & ~(uint32_t)TrackSolo) | (t.solo ? TrackSolo : 0u);
    if (!o || t.minimized != was.minimized) f = (f & ~(uint32_t)TrackMinimized) | (t.minimized ? TrackMinimized : 0u);
    if (f != f0) setU32(h, 4, f);
    if (!o || t.height != was.height) setU32(h, 0x08, t.height > 0 ? (uint32_t)t.height : (video ? 64u : 68u));
    // colour: VEGAS's colour index (vegformat.h). One of VEGAS's colours keeps its index; another is saved as the
    // nearest of them; a track with no colour gets the index VEGAS gives by position.
    if (!o || t.color != was.color) {
        uint32_t ci = (uint32_t)index;
        if (t.color != 0) {
            ci = trackColorIndex(t.color);
            if ((trackColorOfIndex(ci) & 0xFFFFFF) != (t.color & 0xFFFFFF))
                warnCount("%d track colour(s) are not one of VEGAS's track colours; the nearest of them was saved.");
            // an index that already gave this colour is kept (VEGAS counts indexes past 8)
            if (o && trackColorOfIndex(hc->data.u32(0x14)) == trackColorOfIndex(ci)) ci = hc->data.u32(0x14);
        }
        setU32(h, 0x14, ci);
    }
    if (!o) {
        setU32(h, 0x1c, vegId);
    } else if (vegId != o->vegId) {
        setU32(h, 0x1c, vegId);
    }
    return h;
}

// The track's events, with those Reno could not read where they were.
void Writer::trackEvents(const Track& t, const VegTrackOrigin* o, OutNode& out)
{
    if (!t.events.empty() || (o && !o->hiddenEvents.empty())) {
        OutNode el = listNode(G::Events);
        size_t h = 0;
        for (size_t i = 0; i <= t.events.size(); ++i) {
            while (o && h < o->hiddenEvents.size() && o->hiddenEvents[h].before <= i) {
                for (const Chunk* c : o->hiddenEvents[h].chunks)
                    if (hiddenDone_.insert(c).second) el.kids.push_back(copyNode(*c));
                ++h;
            }
            if (i < t.events.size()) el.kids.push_back(event(t.events[i], t));
        }
        out.kids.push_back(std::move(el));
    }
}

// The track's video or audio settings record (levels, pan, compositing mode, bus...).
void Writer::trackSettings(const Track& t, const Chunk& base, const VegTrackOrigin* o, const Track& was, bool video, const std::string& label,
                           size_t fxWritten, size_t chainWritten, OutNode& out)
{
    // sliders in automation mode: TrackFlag in vegformat.h
    const Chunk* hdr = childChunk(base, G::TrackHeader);
    const bool automationMode = o && hdr && (hdr->data.u32(4) & TrackAutomation) != 0;
    const Chunk* dl = childList(base, video ? G::VideoTrackList : G::AudioTrackList);
    const Chunk* dc = dl ? childChunk(*dl, video ? G::VideoTrackData : G::AudioTrackData) : nullptr;
    if (dc) {
        std::string d = bytesOf(dc->data);
        if (video) {
            auto colour = [&](size_t off, uint32_t argb) {
                setF32(d, off, ((argb >> 16) & 0xff) / 255.0f);
                setF32(d, off + 4, ((argb >> 8) & 0xff) / 255.0f);
                setF32(d, off + 8, (argb & 0xff) / 255.0f);
                setF32(d, off + 12, ((argb >> 24) & 0xff) / 255.0f);
            };
            if (!o || t.fadeTop != was.fadeTop) colour(0x30, t.fadeTop);
            if (!o || t.fadeBottom != was.fadeBottom) colour(0x40, t.fadeBottom);
            // compositing mode at 0x64 and the parent's at 0x80: a number, and a plug-in's GUID (see vegformat.h)
            auto setMode = [&](size_t off, const std::string& name, const std::string& wasName, const char* what) {
                if (o && name == wasName) return;
                int32_t mode = 4;
                std::string guid;
                if (!compositeModeOf(name, mode, guid)) {
                    warn(label + ": its " + what + " '" + name + "' is not one of VEGAS's; the file keeps the one it had.");
                    return;
                }
                if (name == "custom") {
                    warn(label + ": its " + what + " Custom (a compositing plug-in) was not saved: Reno keeps no compositing plug-in to "
                         "save with it; the file keeps the mode it had.");
                    return;
                }
                if (d.size() < off + 0x18) return;
                setU32(d, off, (uint32_t)mode);
                d.replace(off + 8, 16, guid.empty() ? std::string(16, '\0') : guidBytes(guid.c_str()));
            };
            setMode(0x64, t.composite, was.composite, "compositing mode");
            // level (Level slider): the trim value at 0x68, or in automation mode the composite level parameter at
            // 0xa4 (see the reader)
            if (!o || t.level != was.level) {
                const float lv = (float)std::max(0.0, std::min(1.0, t.level));
                if (automationMode && d.size() >= 0xa8) {
                    const float trim = dc->data.f32(0x68);
                    setF32(d, 0xa4, trim > 1e-6f ? std::min(1.0f, lv / trim) : lv);
                } else {
                    setF32(d, 0x68, lv);
                }
            }
            if (automationMode && d.size() >= 0xb4 && t.mute != was.mute) setU32(d, 0xb0, t.mute ? 1u : 0u);
            setMode(0x80, t.parentComposite, was.parentComposite, "parent compositing mode");
            // effect count and last-shown effect (VEGAS uses 0 or -1 for none)
            if (dc->data.u32(0xa0) != (uint32_t)fxWritten) setU32(d, 0xa0, (uint32_t)fxWritten);
            const int32_t sel = dc->data.i32(0x9c);
            if (!o) setU32(d, 0x9c, 0);
            else if (fxWritten > 0 && sel >= (int32_t)fxWritten) setU32(d, 0x9c, (uint32_t)(fxWritten - 1));
        } else {
            // volume and pan: trim values (in automation mode the automation values; see the reader)
            const bool automation = automationMode && d.size() >= 0x304;
            if (!o || t.level != was.level) {
                if (automation) {
                    const float trim = dc->data.f32(0x0c);
                    setF32(d, 0x204, (float)(trim > 1e-6f ? t.level / trim : t.level));
                } else {
                    setF32(d, 0x0c, (float)t.level);
                }
            }
            if (!o || t.pan != was.pan) setF32(d, automation ? 0x208 : 0x10, (float)t.pan);
            if (automation && (!o || t.mute != was.mute)) setU32(d, 0x300, t.mute ? 1u : 0u);
            // the pan type and the Invert Phase switch
            if (d.size() >= 0x200) {
                if (!o || t.panType != was.panType) {
                    uint32_t pt = 0;
                    if (panTypeOf(t.panType, pt)) setU32(d, 0x1f8, pt);
                    else warn(label + ": its pan type '" + t.panType + "' is not one of VEGAS's; the file keeps the one it had.");
                }
                if (!o || t.invertPhase != was.invertPhase) setU32(d, 0x1fc, (dc->data.u32(0x1fc) & ~1u) | (t.invertPhase ? 1u : 0u));
            }
            uint32_t bus = 0;
            if (t.output) {
                auto it = busIndex_.find(t.output);
                if (it != busIndex_.end() && it->second <= 26) bus = it->second;
                else warn(label + ": the bus its sound goes to was not saved; in VEGAS it goes to the master.");
            }
            if (!o || bus != dc->data.u32(0x08)) setU32(d, 0x08, bus);
            setU32(d, 0x304, (uint32_t)chainWritten);
        }
        OutNode l = listNode(video ? G::VideoTrackList : G::AudioTrackList);
        l.kids.push_back(chunkNode(video ? G::VideoTrackData : G::AudioTrackData, d));
        out.kids.push_back(std::move(l));
    }
}

// Warns about the parts of the track that a VEGAS project cannot keep.
void Writer::trackWarnings(const Track& t, const VegTrackOrigin* o, const Track& was, const std::string& label)
{
    if (!t.motion.empty())
        warn(label + ": its track motion was not saved: how VEGAS stores a changed track motion is not known.");
    if (!t.parentMotion.empty())
        warn(label + ": its Parent Motion was not saved: how VEGAS stores a changed track motion is not known.");
    if (!t.shadow2d.empty() || !t.glow2d.empty())
        warn(label + ": its Track Motion 2D Shadow / 2D Glow was not saved: no VEGAS project here shows how VEGAS keeps them.");
    if (!t.sends.empty()) warn(label + ": its sends to buses were not saved: no VEGAS project shows how a send is kept.");
    if (t.compositeLevel != 0 && (!o || t.compositeLevel != was.compositeLevel))
        warn(label + ": its compositing parent/child setting was not saved: where VEGAS keeps it is not known.");
    if (t.trackGroup != 0 && (!o || t.trackGroup != was.trackGroup))
        warnCount("%d track(s) in a track group: the group was not saved (no VEGAS project here has one, so where VEGAS keeps it is not known).");
    if (t.adjustment) warn(label + ": it is an adjustment track, which VEGAS Pro 18 does not have; it was saved as an ordinary track.");
    // Reno's automation settings button, mode and values
    if (t.automation || t.automationMode != "read" || t.autoLevel != 1.0 || t.autoPan != 0.0 || t.autoFade != 0.0 || t.autoMute)
        warnCount("%d track(s)' Automation Settings (mode and automation values) were not saved: Reno does not write them into a VEGAS project yet.");
}


// ---------------------------------------------------------------- events
OutNode Writer::event(const Event& e, const Track& t)
{
    const auto* o = originOf<VegEventOrigin>(e.veg, VegOrigin::EventK);
    const bool video = t.type == TrackType::Video;
    const std::string where = (t.name.empty() ? std::string("a ") + (video ? "video" : "audio") + " track" : "track '" + t.name + "'") +
                              strfmt(", event at %.3f s", e.start);
    const Chunk* base = o ? o->chunk : nullptr;
    if (!base) base = eventTemplate(video);
    OutNode out = listNode(G::Event);
    if (!base) return out;
    const Event def;
    const Event& was = o ? o->asRead : def;

    uint64_t vegId = o ? o->vegId : 0;
    if (!vegId || usedEventIds_.count(vegId)) vegId = newEventId(t.type);
    usedEventIds_.insert(vegId);
    eventVegId[e.id] = vegId;
    out.kids.push_back(chunkNode(G::EventData, eventRecord(e, *base, o, was, video, vegId)));
    // the takes
    eventTake(e, t, o, *base, out, vegId);

    // envelopes
    if (!e.envelopes.empty() || (o && !o->hiddenEnvelopes.empty())) {
        OutNode el = listNode(G::EventEnvelopes);
        envelopes(el, e.envelopes, o ? &o->hiddenEnvelopes : nullptr, false, t.type, &e, o ? &was : nullptr, where);
        if (!el.kids.empty()) out.kids.push_back(std::move(el));
    }

    // the frame or sound settings, and the effects
    if (video) eventVideoParts(e, *base, o, was, where, out);
    else eventAudioParts(e, *base, o, was, where, out);
    // Reno's event snap offsets and sync links: no sample has them
    if (e.snapOffset > 0) warnCount("%d event snap offset(s) were not saved: no VEGAS project here has one, so where VEGAS keeps them is not known.");
    if (e.adjustment) warnCount("%d adjustment event(s) were saved as empty events: VEGAS Pro 18 has no adjustment events.");
    if (e.syncLink) warnCount("%d sync link(s) were not saved: no VEGAS project here has one, so how VEGAS keeps them is not known.");
    // anything else the event had
    if (o) {
        for (const auto& k : base->kids) {
            if (!k.isList && (k.id == G::EventData || k.id == G::Take || k.id == G::PanCrop)) continue;
            if (k.isList && (k.type == G::VideoEventList || k.type == G::AudioEventList || k.type == G::EventFx || k.type == G::EventAudioFx ||
                             k.type == G::EventEnvelopes))
                continue;
            out.kids.push_back(copyNode(k));
        }
    }
    return out;
}

// The event record to copy from for an existing event, or a template for a new one.
const Chunk* Writer::eventTemplate(bool video)
{
    const Chunk* base = nullptr;
    base = sameFile([&](const Chunk& c) {
        if (!c.isList || c.type != G::Event) return false;
        const Chunk* d = childChunk(c, G::EventData);
        return d && d->data.u32(0x08) == (video ? 3u : 2u) && childChunk(c, G::Take) && d->data.u32(0) == 0x60;
    });
    if (!base) base = tpl(video ? "videoEvent" : "audioEvent");
    if (!base) {
        const Chunk* tr = tpl(video ? "videoTrack" : "audioTrack");
        const Chunk* el = tr ? childList(*tr, G::Events) : nullptr;
        base = el && !el->kids.empty() ? &el->kids[0] : nullptr;
    }
    return base;
}

// The event's own record: the original with whatever changed since it was read.
std::string Writer::eventRecord(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, bool video, uint64_t vegId)
{
    const Chunk* dc = childChunk(base, G::EventData);
    std::string d = bytesOf(dc->data);
    {
        const uint32_t nameAt = dc->data.u32(0), nameLen = dc->data.u32(0x24);
        if (!o || e.name != was.name) {
            const std::string n = e.name.empty() ? std::string() : utf16(e.name, true);
            const std::string after = bytesOf(dc->data.sub(nameAt + nameLen, 0x48 + 0x10));
            d = d.substr(0, nameAt) + n + after;
            setU32(d, 0x24, (uint32_t)n.size());
            pad8(d);
        }
        const size_t s = (size_t)nameAt + (o && e.name == was.name ? nameLen : (e.name.empty() ? 0 : utf16(e.name, true).size()));
        uint8_t f0 = (uint8_t)d[4], f1 = (uint8_t)d[5];
        if (!o) f0 &= (uint8_t)~0x10;
        if (!o || e.mute != was.mute) f0 = (uint8_t)((f0 & ~0x02) | (e.mute ? 0x02 : 0));
        if (!o || e.locked != was.locked) f0 = (uint8_t)((f0 & ~0x08) | (e.locked ? 0x08 : 0));
        if (!o || e.loop != was.loop) f1 = (uint8_t)((f1 & ~0x01) | (e.loop ? 0x01 : 0));
        if (!o) f1 &= (uint8_t)~0x02;           // not normalised
        if (!video && (!o || e.normalize != was.normalize)) f1 = (uint8_t)((f1 & ~0x02) | (e.normalize ? 0x02 : 0));
        d[4] = (char)f0;
        d[5] = (char)f1;
        if (!o || e.start != was.start) setU64(d, 0x10, (uint64_t)toTicks(e.start));
        if (!o || e.length != was.length) setU64(d, 0x18, (uint64_t)toTicks(e.length));
        if (!o || e.rate != was.rate) setF64(d, 0x28, e.rate);
        if (!o || vegId != o->vegId) setU64(d, 0x40, vegId);
        // gain: VEGAS stores a normalised event's normalising gain separately (Event::normalizeGain)
        if (!o || e.gain != was.gain) setF32(d, s + 0x0c, (float)e.gain);
        if (!video && e.normalize && (!o || e.normalize != was.normalize || e.normalizeGain != was.normalizeGain))
            setF32(d, s + 0x10, (float)e.normalizeGain);
        if (!o || e.fadeIn != was.fadeIn) setU64(d, s + 0x28, (uint64_t)toTicks(e.fadeIn));
        if (!o || e.fadeOut != was.fadeOut) setU64(d, s + 0x30, (uint64_t)toTicks(e.fadeOut));
        if (!o || e.fadeInCurve != was.fadeInCurve) setU32(d, s + 0x14, (uint32_t)curveToVegas(e.fadeInCurve));
        if (!o || e.fadeOutCurve != was.fadeOutCurve) setU32(d, s + 0x18, (uint32_t)curveToVegas(e.fadeOutCurve));
        // curve the previous event fades out by under this fade-in (FadeIn.ReciprocalCurve; when not given, the
        // mirror of the fade-in, as VEGAS makes it)
        if (!o || e.fadeInRecipCurve != was.fadeInRecipCurve) {
            const int c = e.fadeInRecipCurve >= 0 ? e.fadeInRecipCurve
                        : e.fadeInCurve == CurveFast ? (int)CurveSlow : e.fadeInCurve == CurveSlow ? (int)CurveFast : e.fadeInCurve;
            setU32(d, s + 0x1c, (uint32_t)curveToVegas(c));
        }
        // "in a group" is corrected once groups are known (see groups())
    }
    return d;
}

// Video events: the frame settings, the effects and the pan/crop.
void Writer::eventVideoParts(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, const std::string& where, OutNode& out)
{
    size_t fxWritten = 0;
    OutNode fxList = listNode(G::EventFx);
    bool haveFx = false;
    bool hasOwn = false;
    for (const auto& f : e.fx) {
        const auto* fo = originOf<VegFxOrigin>(f.veg, VegOrigin::FxK);
        if (f.kind == "ofx" && !(fo && fo->mediaFx)) hasOwn = true;
        if (f.kind == "audio") warn(where + ": audio effects on a video event were not saved.");
    }
    if (!hasOwn && !(o && !o->hiddenFx.empty()))
        for (const auto& f : e.fx)
            if (f.kind == "builtin") warn(where + ": Reno's own \"" + f.name + "\" effect was not saved (a VEGAS project cannot hold it).");
    if (hasOwn || (o && !o->hiddenFx.empty())) {
        videoFx(fxList, e.fx, o ? &o->hiddenFx : nullptr, where, &fxWritten);
        haveFx = !fxList.kids.empty();
    }
    const Chunk* vl = childList(base, G::VideoEventList);
    const Chunk* vd = vl ? childChunk(*vl, G::VideoEventData) : nullptr;
    if (vd) {
        std::string v = bytesOf(vd->data);
        if (!o || e.resample != was.resample) setU32(v, 4, (vd->data.u32(4) & ~0x200u) | (e.resample == "off" ? 0u : 0x200u));
        // last-shown effect: -1 for none
        const int32_t sel = vd->data.i32(0x68);
        if (!o) setU32(v, 0x68, 0xFFFFFFFFu);
        else if (sel > (int32_t)fxWritten) setU32(v, 0x68, (uint32_t)fxWritten);    // the effects, or pan/crop after them
        OutNode l = listNode(G::VideoEventList);
        l.kids.push_back(chunkNode(G::VideoEventData, v));
        out.kids.push_back(std::move(l));
    }
    if (haveFx) out.kids.push_back(std::move(fxList));
    eventPanCrop(e, base, o, was, where, out);
    if (!e.transition.kind.empty()) warnCount("%d transition(s) were not saved: no VEGAS project here has one, so how VEGAS keeps them is not known.");
    if (e.resample == "force" || e.resample == "optical") warnCount("%d event resample mode(s) (Force / Optical flow) were not saved: where VEGAS keeps them is not known.");
    if (!e.keepAspect) warnCount("%d event(s) with Maintain Aspect Ratio off were not saved: where VEGAS keeps it is not known.");
    if (e.undersample != 1.0) warnCount("%d event undersample rate(s) were not saved: where VEGAS keeps them is not known.");
    if (e.maskOn || !e.mask.empty()) warnCount("%d Event Pan/Crop mask(s) were not saved: no VEGAS project here has one, so how VEGAS keeps them is not known.");
    if (e.stretchToFill) warnCount("%d Event Pan/Crop \"Stretch to fill frame\" setting(s) were not saved: where VEGAS keeps it is not known.");
    bool smooth = false;
    for (const PanCropKey& k : e.panCrop) smooth = smooth || k.smoothness != 0;
    if (smooth) warnCount("%d event(s)' Pan/Crop keyframe Smoothness was not saved: where VEGAS keeps it is not known.");
}

// Event Pan/Crop keyframes, written into the original record when there is one.
void Writer::eventPanCrop(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, const std::string& where, OutNode& out)
{
    const Chunk* pc = childChunk(base, G::PanCrop);
    if (o && samePanCrop(e.panCrop, was.panCrop)) {
        if (pc) out.kids.push_back(copyNode(*pc));
    } else if (!e.panCrop.empty()) {
        const MediaItem* m = p_.findMedia(e.mediaId);
        int mw = m ? m->info.width : 0, mh = m ? m->info.height : 0;
        if (pc && o) {
            // the media size as VEGAS had it
            PanCropRecord r;
            if (readPanCrop(pc->data, r) && r.mediaW > 0 && r.mediaH > 0) { mw = r.mediaW; mh = r.mediaH; }
        }
        const Chunk* tp = (pc && o) ? pc : sameFile([](const Chunk& c) { return !c.isList && c.id == G::PanCrop; });
        if (!tp) tp = tpl("panCrop");
        if (mw <= 0 || mh <= 0 || !tp) {
            warn(where + ": its pan/crop was not saved: the media's picture size is not known.");
        } else {
            // LIST EPCP { EPCS, KFRS, LIST POSL { POSK... }, LIST MSKL } (classic 32-bit RIFF, see vegformat.h)
            const Bytes& b = tp->data;
            std::string epcs, kfrs, mskl;
            std::vector<std::string> oldKeys;       // the original's keys (84 bytes each, after their header)
            size_t off = 12;
            const size_t end = std::min<size_t>(b.size(), 8 + (size_t)b.u32(4));
            while (off + 8 <= end) {
                const uint32_t cc = b.be32(off), sz = b.u32(off + 4);
                const std::string whole = bytesOf(b.sub(off, 8 + (size_t)sz + (sz & 1)));
                if (cc == 0x45504353u) epcs = whole;
                else if (cc == 0x4b465253u) kfrs = whole;
                else if (cc == 0x4c495354u && b.be32(off + 8) == 0x4d534b4cu) mskl = whole;
                else if (cc == 0x4c495354u && b.be32(off + 8) == 0x504f534cu && o) {
                    for (size_t k = off + 12; k + 8 + 84 <= off + 8 + sz; k += 8 + 84)
                        if (b.be32(k) == 0x504f534bu && b.u32(k + 4) == 84) oldKeys.push_back(bytesOf(b.sub(k + 8, 84)));
                }
                off += 8 + (size_t)sz + (sz & 1);
            }
            if (epcs.size() >= 8 + 0x34) {
                setU32(epcs, 8 + 0x2c, (uint32_t)mw);
                setU32(epcs, 8 + 0x30, (uint32_t)mh);
            }
            std::string posl = "LIST";
            addU32(posl, 0);
            posl += "POSL";
            for (size_t ki = 0; ki < e.panCrop.size(); ++ki) {
                const PanCropKey& k = e.panCrop[ki];
                std::string key = "POSK";
                addU32(key, 84);
                addU64(key, (uint64_t)toTicks(k.time));
                // the 8 bytes after the time are unknown: a key that existed keeps them; 0 in most keys
                if (ki < oldKeys.size()) key += oldKeys[ki].substr(8, 8);
                else key.append(8, '\0');
                // key type (VideoKeyframeType)
                const uint32_t type = (uint32_t)keyCurveToVegas(k.curve);
                addU32(key, type);
                const double cx = k.cx * mw, cy = k.cy * mh;
                addF64(key, k.w * mw);
                addF64(key, k.h * mh);
                addF64(key, cx);
                addF64(key, cy);
                addF64(key, -k.rotation * kPi / 180.0);
                // rotation centre
                addF64(key, cx + k.pivotX * mw);
                addF64(key, cy + k.pivotY * mh);
                addF64(key, 0.0);
                posl += key;
            }
            setU32(posl, 4, (uint32_t)(posl.size() - 8));
            std::string body = "EPCP" + epcs + kfrs + posl + mskl;
            std::string riff = "LIST";
            addU32(riff, (uint32_t)body.size());
            riff += body;
            out.kids.push_back(chunkNode(G::PanCrop, riff));
        }
    }
}

// Audio events: effect chain, and channel, time stretch and pitch settings.
void Writer::eventAudioParts(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, const std::string& where, OutNode& out)
{
    const Chunk* al = childList(base, G::EventAudioFx);
    const Chunk* origChain = o && al ? childChunk(*al, G::EventAudioFx) : nullptr;
    FxChain audioSlots;
    for (const auto& f : e.fx) {
        if (f.kind == "audio") audioSlots.push_back(f);
        else warn(where + ": video effects on an audio event were not saved.");
    }
    const bool same = o && sameAudioChain(audioSlots, origChain);
    std::string chain;
    size_t written = 0;
    if (same) {
        if (origChain) { chain = bytesOf(origChain->data); written = audioSlots.size(); }
    } else if (!audioSlots.empty()) {
        std::string c;
        if (audioChain(audioSlots, origChain ? &origChain->data : nullptr, "eventAudioChain", c, &written, where) && written > 0) {
            addU32(c, 0);
            chain = c;
        }
    }
    const Chunk* adl = childList(base, G::AudioEventList);
    const Chunk* ad = adl ? childChunk(*adl, G::AudioEventData) : nullptr;
    if (ad) {
        std::string a = bytesOf(ad->data);
        if (!o || e.channels != was.channels) {
            uint32_t ch = 1000;
            if (e.channels == "right") ch = 1001;
            else if (e.channels == "left") ch = 1002;
            else if (e.channels == "combine") ch = 1005;
            else if (e.channels == "swap") ch = 1006;
            setU32(a, 0x04, ch);
        }
        // Time stretch / pitch shift (see the reader): flags at 0x08 (0x01 invert phase, 0x02 lock to stretch, bits
        // 2-3 method, 0x10 preserve formants), Classic preset at 0x0c, elastique mode at 0x10, pitch change at 0x14,
        // formant shift at 0x1c. Only what changed is written.
        const bool none = e.stretchMethod == "none";
        const uint32_t flags0 = ad->data.u32(0x08);
        uint32_t flags = flags0;
        if (!o || e.invertPhase != was.invertPhase) flags = (flags & ~0x01u) | (e.invertPhase ? 0x01u : 0u);
        if (!o || e.stretchMethod != was.stretchMethod) {
            const uint32_t method = none ? 3u : (e.stretchMethod == "classic" ? 0u : 2u);
            // ACID-style method, read as Classic, stays ACID-style while Reno did not change the method
            if (!(o && e.stretchMethod == "classic" && ((flags0 >> 2) & 3) == 1)) flags = (flags & ~0x0cu) | (method << 2);
        }
        if (!o || e.preserveFormants != was.preserveFormants) flags = (flags & ~0x10u) | (e.preserveFormants ? 0x10u : 0u);
        // pitch: kept (moved by the pitch change), or locked to the rate ("Lock to stretch"); None has neither (the
        // file keeps what it had)
        double pitch = ad->data.f64(0x14);
        if (!none && (!o || e.keepPitch != was.keepPitch || e.pitch != was.pitch || e.rate != was.rate || e.stretchMethod != was.stretchMethod)) {
            if (e.keepPitch) {
                flags &= ~0x02u;
                pitch = e.pitch;
            } else if (e.pitch == 0) {
                flags |= 0x02u;
                pitch = 0;
            } else {
                // rate pitch plus semitones: VEGAS can only keep that as a kept pitch with the sum
                flags &= ~0x02u;
                pitch = 12.0 * std::log2(e.rate > 0 ? e.rate : 1.0) + e.pitch;
            }
        }
        if (flags != flags0) setU32(a, 0x08, flags);
        if (pitch != ad->data.f64(0x14)) setF64(a, 0x14, pitch);
        if (!o || e.stretchMode != was.stretchMode || e.stretchMethod != was.stretchMethod) {
            if (e.stretchMethod == "classic") {
                const ClassicPreset* pr = classicPreset(e.stretchMode);
                if (pr) setU32(a, 0x0c, (uint32_t)(pr - classicPresets().data()));
                else warn(where + ": its Classic stretch preset '" + e.stretchMode + "' is not one of VEGAS's (A01 to A19); the file keeps the one it had.");
            } else if (e.stretchMethod == "elastique") {
                const auto& modes = elastiqueModes();
                size_t i = 0;
                while (i < modes.size() && modes[i].first != e.stretchMode) ++i;
                if (i < modes.size()) setU32(a, 0x10, (uint32_t)i);
                else warn(where + ": its élastique mode '" + e.stretchMode + "' is not one of VEGAS's; the file keeps the one it had.");
            }
        }
        if ((!o || e.formant != was.formant) && e.formant != ad->data.f64(0x1c)) setF64(a, 0x1c, e.formant);
        setU32(a, 0x24, chain.empty() ? 0u : 1u);
        OutNode l = listNode(G::AudioEventList);
        l.kids.push_back(chunkNode(G::AudioEventData, a));
        out.kids.push_back(std::move(l));
    }
    if (!chain.empty()) {
        OutNode l = listNode(G::EventAudioFx);
        l.kids.push_back(chunkNode(G::EventAudioFx, chain));
        out.kids.push_back(std::move(l));
    }
}


// The takes: which media the event plays, and from where: the active take and the event's other takes
// (Event::takes). VEGAS stores each take's offset in timeline time (media position / rate), counted from the start
// of the media record the take names (a subclip's start, or for a reversed subclip its end). A take unchanged since
// reading is written back as it was; takes keep their file order (new ones at the end), and the event's record says
// which is active.
namespace {
bool sameTake(const Take& a, const Take& b)
{
    // offsets in a file are 100 ns ticks of timeline time: a value read back can differ by that much
    return a.mediaId == b.mediaId && a.reverse == b.reverse && std::fabs(a.offset - b.offset) < 1e-5;
}
}  // namespace

void Writer::eventTake(const Event& e, const Track& t, const VegEventOrigin* o, const Chunk& base, OutNode& out, uint64_t vegId)
{
    (void)vegId;
    (void)t;
    const std::vector<const Chunk*> takes = [&] {
        std::vector<const Chunk*> v;
        for (const auto& k : base.kids)
            if (!k.isList && k.id == G::Take) v.push_back(&k);
        return v;
    }();
    if (takes.empty()) return;
    const Event def;
    const Event& was = o ? o->asRead : def;
    const double rate = e.rate > 0 ? e.rate : 1.0;
    const bool sameTiming = o && e.rate == was.rate && e.length == was.length;

    // Where a take of 'k' goes: the media record and offset in it (timeline time), and the name for a new take.
    // 'wasRec' is its previous record (0: none); it stays on that record where it can (a subclip stays a subclip).
    struct Place { uint32_t rec = 0; double offT = 0; std::string name; };
    auto place = [&](const Take& k, const Take* wasK, uint32_t wasRec, Place& w) -> bool {
        if (wasK && wasRec && k.mediaId == wasK->mediaId && k.reverse == wasK->reverse && writtenRecords_.count(wasRec)) {
            const VegMediaRecord* rec = nullptr;
            for (const auto& r : po_->media)
                if (r.vegId == wasRec) { rec = &r; break; }
            if (rec) {
                if (rec->kind == VegMediaRecord::Subclip && !rec->subReversed && k.offset >= rec->subStart) {
                    w.rec = rec->vegId;
                    w.offT = (k.offset - rec->subStart) / rate;
                    return true;
                }
                if (rec->kind == VegMediaRecord::Subclip && rec->subReversed) {
                    const double o2 = reversedClipEnd(rec->subStart, rec->subLen, p_.findMedia(k.mediaId) ? &p_.findMedia(k.mediaId)->info : nullptr) - k.offset - e.length * rate;
                    if (o2 >= 0 && k.offset >= rec->subStart) {
                        w.rec = rec->vegId;
                        w.offT = o2 / rate;
                        return true;
                    }
                } else if (rec->kind != VegMediaRecord::Subclip) {
                    w.rec = rec->vegId;
                    w.offT = k.offset / rate;
                    return true;
                }
            }
        }
        const MediaItem* m = p_.findMedia(k.mediaId);
        if (!m) {
            warn("An event uses media that is not in the project; it was saved without it.");
            return false;
        }
        if (!k.reverse) {
            w.rec = fileRecordFor(*m);
            w.offT = k.offset / rate;
            w.name = m->generated ? m->name : stemOf(m->path);
            return w.rec != 0;
        }
        SubclipRef sc;
        const double needStart = k.offset, needEnd = k.offset + e.length * rate;
        w.rec = reversedSubclipFor(*m, needStart, needEnd, sc);
        if (w.rec) {
            w.offT = std::max(0.0, (reversedClipEnd(sc.start, sc.len, &m->info) - k.offset - e.length * rate) / rate);
            w.name = fileNameOf(m->path) + " - subclip (reversed)";
            return true;
        }
        warn(strfmt("Reversed event at %.3f s: a take of it was saved playing forwards (a reversed subclip could not be made for it).", e.start));
        w.rec = fileRecordFor(*m);
        w.offT = k.offset / rate;
        w.name = stemOf(m->path);
        return w.rec != 0;
    };

    // Which file take each of the event's takes is. The active one stays active while its media and direction are
    // the same; otherwise (VEGAS's T: another take made active) it is matched to the other takes as they were read.
    const size_t origActive = o ? std::min(o->activeTake, takes.size() - 1) : 0;
    struct Item { Take value; int orig = -1; bool active = false; };
    Take actValue;
    actValue.mediaId = e.mediaId;
    actValue.offset = e.offset;
    actValue.reverse = e.reverse;
    std::vector<Item> items;
    std::vector<bool> used(takes.size(), false);
    {
        Item a;
        a.value = actValue;
        a.active = true;
        if (o) {
            if (e.mediaId == was.mediaId && e.reverse == was.reverse) a.orig = (int)origActive;
            else
                for (size_t i = 0; i < takes.size() && i < o->takesRead.size(); ++i)
                    if (i != origActive && o->takesOk[i] && sameTake(o->takesRead[i], actValue)) { a.orig = (int)i; break; }
            if (a.orig >= 0) used[(size_t)a.orig] = true;
        }
        items.push_back(a);
    }
    for (const Take& k : e.takes) {
        Item it;
        it.value = k;
        if (o)
            for (size_t i = 0; i < takes.size() && i < o->takesRead.size(); ++i)
                if (!used[i] && o->takesOk[i] && sameTake(o->takesRead[i], k)) { it.orig = (int)i; used[i] = true; break; }
        items.push_back(it);
    }
    // an active take of new media takes the old active one's place (unless another take took it)
    if (o && items[0].orig < 0 && !used[origActive]) {
        items[0].orig = (int)origActive;
        used[origActive] = true;
    }
    if (!o && !items.empty()) items[0].orig = 0;     // new event: from the template's take
    // Order: the other takes as Reno has them, the active one where it was in the file (the file's order when nothing
    // moved; reading back gives Reno's takes in Reno's order either way).
    std::vector<const Item*> order;
    for (size_t i = 1; i < items.size(); ++i) order.push_back(&items[i]);
    order.insert(order.begin() + (std::ptrdiff_t)std::min(origActive, order.size()), &items[0]);
    uint32_t activeIdx = 0, written = 0;
    for (const Item* it : order) {
        const Chunk& tk = *takes[(size_t)(it->orig >= 0 ? it->orig : (int)origActive)];
        const bool fromOrig = o && it->orig >= 0;
        const Take* wasK = fromOrig && (size_t)it->orig < o->takesRead.size() && o->takesOk[(size_t)it->orig] ? &o->takesRead[(size_t)it->orig] : nullptr;
        const uint32_t wasRec = fromOrig ? tk.data.u32(0x24) : 0;
        // unchanged: written back as it was (while its media is)
        if (wasK && sameTiming && sameTake(*wasK, it->value) && (it->value.name.empty() || it->value.name == wasK->name || it->active) &&
            writtenRecords_.count(wasRec)) {
            if (it->active) activeIdx = written;
            out.kids.push_back(copyNode(tk));
            ++written;
            continue;
        }
        // another take whose media is gone goes with it
        if (wasK && !it->active && sameTiming && sameTake(*wasK, it->value) && !writtenRecords_.count(wasRec)) continue;
        Place w;
        if (!place(it->value, wasK, wasRec, w)) continue;
        std::string d = bytesOf(tk.data);
        setU32(d, 0x24, w.rec);
        setU64(d, 0x10, (uint64_t)toTicks(w.offT));
        // take length: the event's length when the take was made; follows the event where they were the same
        if (!fromOrig || tk.data.i64(0x18) == toTicks(was.length)) setU64(d, 0x18, (uint64_t)toTicks(e.length));
        // its name: Reno's (another take's), or the new media's
        std::string name = it->active ? std::string() : it->value.name;
        if (name.empty() && (!fromOrig || w.rec != wasRec)) name = w.name;
        if (!name.empty() && (!wasK || name != wasK->name)) {
            const std::string n = utf16(name, true);
            d = d.substr(0, tk.data.u32(0)) + n;
            setU32(d, 0x20, (uint32_t)n.size());
            pad8(d);
        }
        if (it->active) activeIdx = written;
        out.kids.push_back(chunkNode(G::Take, d));
        ++written;
    }
    if (!out.kids.empty() && out.kids[0].data.size() >= 0x24 && Bytes((const uint8_t*)out.kids[0].data.data(), out.kids[0].data.size()).u32(0x20) != activeIdx)
        setU32(out.kids[0].data, 0x20, activeIdx);
}

}  // namespace vegwrite
}  // namespace reno
