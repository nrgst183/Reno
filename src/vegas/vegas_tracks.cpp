// Reads the tracks, events and envelopes of a VEGAS Pro project (.veg); see vegas.cpp.
#include "vegas_internal.h"
#include "../audiofx/audioplugin.h"

namespace reno {
namespace vegread {

// ---------------------------------------------------------------- envelopes
// VEGAS's curve types (CurveType) mapped to Reno's.
int curveFromVegas(int32_t c, bool* known)
{
    if (known) *known = true;
    switch (c) {
    case 1: return CurveLinear;
    case 2: return CurveFast;
    case -2: return CurveSlow;
    case 4: return CurveSmooth;
    case -4: return CurveSharp;
    case 0: return CurveHold;
    default: break;
    }
    if (known) *known = false;
    return CurveLinear;
}

// A list of envelopes (header chunk and points chunk each). Kinds Reno has become Envelopes; the rest (effect
// automation etc.) go to 'hidden' and are written back unchanged.
// The slot of a type 301 envelope (see EnvelopeRecord): the plug-in its CLSID names, preferably at the place the
// envelope says; null if the chain has no such VST plug-in. Also gives the setting's name.
static const FxSlot* pluginOfEnvelope(const FxChain& chain, const EnvelopeRecord& e, std::string& paramName)
{
    const FxSlot* found = nullptr;
    for (size_t i = 0; i < chain.size(); ++i) {
        const FxSlot& s = chain[i];
        const auto* o = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK);
        if (s.kind != "audio" || s.audio.format != "VST2" || !o || o->entryLen < 16) continue;
        if (o->chunk->data.sub(o->entryOff, 16).size() != 16 || std::memcmp(o->chunk->data.data() + o->entryOff, e.plugClsid.data(), 16) != 0) continue;
        if (!found || i == e.plugSlot) found = &s;
    }
    if (!found) return nullptr;
    paramName = strfmt("%u", e.plugParam);
    // Reno's own eFX effects know their settings' names (the real ones are the same ones)
    if (efxReproducedId(found->audio.uid)) {
        std::string err;
        if (auto pl = efxCreate(found->audio, &err)) {
            const std::string n = pl->paramName((int)e.plugParam);
            if (!n.empty() && e.plugParam < (uint32_t)pl->paramCount()) paramName = n;
        }
    }
    return found;
}

bool Importer::readEnvelopeList(const Chunk& list, bool track, TrackType type, const std::string& label, Envelopes& out,
                                std::vector<VegHidden>& hidden, const FxChain* chain)
{
    const Chunk* header = nullptr;
    for (const auto& k : list.kids) {
        if (!k.isList && k.id == G::EnvelopeHeader) { header = &k; continue; }
        const Chunk* hc = header;
        header = nullptr;
        auto hide = [&]() {
            VegHidden h;
            h.before = out.size();
            if (hc) h.chunks.push_back(hc);
            h.chunks.push_back(&k);
            hidden.push_back(h);
        };
        if (k.isList || k.id != G::EnvelopePoints) { hide(); unknown(track ? "tracks" : "events"); continue; }
        EnvelopeRecord e;
        if (!readEnvelope(k.data, e)) {
            notes_.add(label + ": an envelope could not be read; it is kept in the file but not used.");
            hide();
            continue;
        }
        Envelope env;
        uint64_t target = 0;
        const bool audio = type == TrackType::Audio;
        if (e.type == EnvVolume && audio) env.kind = "volume";
        else if (e.type == EnvPan && audio) env.kind = "pan";
        else if (e.type == EnvComposite && !audio) env.kind = "composite";
        else if (e.type == EnvMute && track) env.kind = "mute";
        else if (e.type == EnvFadeToColor && track && !audio) env.kind = "fadecolor";
        else if (e.type == 32 && track && !audio) env.kind = "motionblur";       // the video output's Motion Blur Length
        else if (e.type == 33 && track && !audio) env.kind = "supersample";      // and Video Supersampling
        else if (e.type == EnvVelocity && !track) env.kind = "velocity";
        else if (e.type >= EnvBusA && e.type <= EnvBusZ && track && audio) {
            auto it = busByIndex_.find(e.type - EnvBusA + 1);
            if (it != busByIndex_.end()) {
                env.kind = "send";
                target = it->second;
            }
        }
        // a VST plug-in's setting in this track's chain (percent in VEGAS, 0..1 in Reno)
        double scale = 1.0;
        if (env.kind.empty() && e.type == EnvPlugin && chain && track && type == TrackType::Audio) {
            std::string pn;
            if (const FxSlot* s = pluginOfEnvelope(*chain, e, pn)) {
                env.kind = "fx";
                target = s->id;
                env.param = pn;
                scale = 0.01;
            }
        }
        if (env.kind.empty()) {
            notes_.add(strfmt("%s: its %s envelope (%zu point%s) is not used by Reno (it is kept in the file).", label.c_str(),
                              envelopeName(e.type).c_str(), e.points.size(), e.points.size() == 1 ? "" : "s"));
            hide();
            continue;
        }
        env.target = target;
        bool allKnown = true;
        for (const auto& p : e.points) {
            EnvPoint pt;
            pt.time = p.time;
            pt.value = p.value * scale;
            bool known = true;
            pt.curve = curveFromVegas(p.curve, &known);
            allKnown = allKnown && known;
            env.points.push_back(pt);
        }
        if (!allKnown) notes_.add(label + ": an envelope has points with a curve Reno does not know; they are straight lines in Reno.");
        std::stable_sort(env.points.begin(), env.points.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
        auto o = origin<VegEnvelopeOrigin>(&k);
        o->header = hc;
        o->vegType = e.type;
        o->asRead = env;
        env.veg = o;
        out.push_back(env);
    }
    return true;
}

// Audio track settings record: volume, pan, pan type, destination bus and sends.
void Importer::readAudioTrackSettings(const Chunk& tc, Track& t, const std::string& label, bool automation, const bool* hasEnvelope)
{
if (const Chunk* l = childList(tc, G::AudioTrackList)) {
    if (const Chunk* d = childChunk(*l, G::AudioTrackData)) {
        // u32 0x310, u32 -1, u32 destination bus (0 master, n = Bus n with A = 1), f32 volume, f32 pan (-1 left .. 1
        // right), then per-bus send settings: 0x20..0x11f pairs (6, 0), 0x120..0x1ef pairs (1, 0), 0x200 = 0x37,
        // 0x204 = 1.0, 0x210..0x2ff 1.0 each. These are identical in every sample track, so what a set send looks
        // like is unknown; a track with other values is reported. u32 at 0x304: audio effect count.
        // From vegas180.exe's writer: u32 at 0x1f8 pan type, u32 at 0x1fc Invert Phase (bit 0x01), f32 at 0x204 and
        // 0x208 automation volume and pan, 26 f32 at 0x290 (per bus, 1.0), u32 at 0x300 automation mute (0 / 1).
        const Bytes& a = d->data;
        const float vol = a.f32(0x0c), pan = a.f32(0x10);
        if (std::isfinite(vol) && vol >= 0 && vol < 64) t.level = vol;
        if (std::isfinite(pan) && pan >= -1.001f && pan <= 1.001f) t.pan = std::max(-1.0, std::min(1.0, (double)pan));
        if (a.size() >= 0x200) {
            const std::string pt = panTypeName(a.u32(0x1f8));
            if (!pt.empty()) t.panType = pt;
            else notes_.add(strfmt("%s: its pan type (%u) is not one Reno knows; Reno uses Add Channels (the file keeps it).", label.c_str(), a.u32(0x1f8)));
            t.invertPhase = (a.u32(0x1fc) & 1) != 0;
        }
        if (automation && a.size() >= 0x304) {
            const float av = a.f32(0x204), ap = a.f32(0x208);
            if (!hasEnvelope[0] && std::isfinite(av) && av >= 0 && av < 64) t.level *= av;
            if (!hasEnvelope[1] && std::isfinite(ap) && ap >= -1.001f && ap <= 1.001f) t.pan = std::max(-1.0, std::min(1.0, (double)ap));
            if (!hasEnvelope[2] && a.u32(0x300) != 0) t.mute = true;
        }
        const uint32_t bus = a.u32(0x08);
        if (bus != 0) {
            auto it = busByIndex_.find(bus);
            if (it != busByIndex_.end()) t.output = it->second;
            else notes_.add(strfmt("%s: its sound goes to a bus (%u) that is not in the project; Reno sends it to the master.", label.c_str(), bus));
        }
        bool sendsAsUsual = a.size() >= 0x300;
        for (size_t o = 0x20; sendsAsUsual && o < 0x120; o += 8) sendsAsUsual = a.u32(o) == 6 && a.u32(o + 4) == 0;
        for (size_t o = 0x120; sendsAsUsual && o < 0x1f0; o += 8) sendsAsUsual = a.u32(o) == 1 && a.u32(o + 4) == 0;
        for (size_t o = 0x210; sendsAsUsual && o < 0x300; o += 4) sendsAsUsual = a.f32(o) == 1.0f;
        if (!sendsAsUsual)
            notes_.add(label + ": its sends to buses are set in a way that could not be read; Reno sends to no bus (the file keeps them).");
    }
}
}

// Video track settings record: fade colours, compositing modes and level.
void Importer::readVideoTrackSettings(const Chunk& tc, Track& t, const std::string& label, bool automation, const bool* hasEnvelope)
{
if (const Chunk* l = childList(tc, G::VideoTrackList)) {
    if (const Chunk* d = childChunk(*l, G::VideoTrackData)) {
        // u32 0xb4; at 0x30 and 0x40 two colours of four floats (r, g, b, a), taken as the top and bottom colours of
        // the fade-to-colour envelope (white and black in every sample; the order is blue, green, red, alpha: Coverage run 20261105 set orange and read 0,128,255). At 0x64 the
        // compositing mode: u32 mode, f32 (1.0), compositor plug-in GUID (0x6c), u32; at 0x80 the same four for the
        // Parent Composite Mode. u32 at 0xa0: video effect count; 0x9c: last-shown effect (-1 none). Mode numbers
        // and GUIDs are VEGAS 18's (see compositeModeName in vegformat.h).
        const Bytes& v = d->data;
        auto colour = [&](size_t off) -> uint32_t {
            auto ch = [&](size_t o) {
                const float f = v.f32(off + o);
                return (uint32_t)std::lround(std::max(0.0f, std::min(1.0f, std::isfinite(f) ? f : 0.0f)) * 255.0f);
            };
            return (ch(12) << 24) | (ch(8) << 16) | (ch(4) << 8) | ch(0);
        };
        t.fadeTop = colour(0x30);
        t.fadeBottom = colour(0x40);
        // bit 0x100 of the word at 0x0c (with 0x20 child, 0x40 parent): Bypass Motion Blur (Coverage run 20261105)
        t.bypassMotionBlur = (v.u32(0x0c) & 0x100) != 0;
        auto mode = [&](size_t off, const char* what) {
            const int32_t m = v.i32(off);
            const std::string g = v.guid(off + 8);
            const std::string name = compositeModeName(m, g);
            if (!name.empty()) return name;
            notes_.add(label + ": its " + what + " (" + (m == -1 ? "compositor {" + g + "}" : strfmt("mode %d", m)) +
                       ") is not one Reno knows; Reno shows it as Source Alpha (the file keeps it).");
            return std::string("srcalpha");
        };
        t.composite = mode(0x64, "compositing mode");
        // The f32 after the mode is the track's trim level (1.0 = 100 % in every sample). In automation mode it is
        // multiplied by the composite level parameter at 0xa4 (vegas180.exe parameter 0x15) and the mute parameter
        // at 0xb0.
        {
            const float lv = v.f32(0x68);
            if (std::isfinite(lv) && lv >= 0 && lv <= 1.0001f) t.level = std::min(1.0, (double)lv);
            if (automation && v.size() >= 0xb4) {
                const float al = v.f32(0xa4);
                if (!hasEnvelope[0] && std::isfinite(al) && al >= 0 && al <= 1.0001f) t.level *= std::min(1.0, (double)al);
                if (!hasEnvelope[2] && v.u32(0xb0) != 0) t.mute = true;
            }
        }
        if (v.size() >= 0x98) t.parentComposite = mode(0x80, "parent compositing mode");
        // parent/child (Track.CompositeNestingLevel, Settings Matrix run 20261028: nesting video track 2 under track 1 set
        // u32 0x20 at 0x0c of track 2's record, 0x40 at 0x0c of track 1's (it has a child) and u32 1 at 0xac of track 2's)
        // u32 0xac holds the nesting level itself (a grandchild has 2; Coverage run 2, 20261105)
        if (v.size() >= 0xb0 && v.u32(0xac) != 0) t.compositeLevel = (int)std::min<uint32_t>(v.u32(0xac), 8);
        if (t.composite == "custom")
            notes_.add(label + ": its compositing mode is Custom (a compositing plug-in), which Reno cannot host; it shows Source "
                       "Alpha (the file keeps it).");
    }
}
}

// ---------------------------------------------------------------- tracks
// A track list holds: the header chunk (TrackFlag in vegformat.h: flags, height, kind, name, colour, id), the
// event list, the envelopes, the video or audio settings, and effect and track motion lists.
// Flags confirmed in vegas180.exe: 0x02 mute, 0x01 solo, 0x10 selected. Height is in Reno timeline units, whose
// defaults (64 video, 68 audio) are VEGAS's.
//
// Automation mode (TrackFlag TrackAutomation): vegas180.exe keeps two values per slider, the trim value (audio
// volume and pan at 0x0c and 0x10 of the settings, video level at 0x68) and the automation value (saved at
// 0x204/0x208 volume and pan and 0x300 mute for audio, 0xa4 composite level and 0xb0 mute for video), and uses
// the automation ones only in automation mode. No sample uses it; Reno takes the trim value, times the
// automation value where the track has no envelope of that kind.
// One track motion key in VEGAS's units (see TrackMotionRecord) as Reno's: the frame's middle as a fraction of the frame (y down), sizes as
// fractions of the frame, degrees. The position unit is half the frame height.
static MotionKey motionKeyOf(const TrackMotionKey& k, double W, double H, double asp)
{
    const double deg = 180.0 / kPi;
    MotionKey q;
    q.time = std::max(0.0, k.time);
        q.x = 0.5 + k.posX / (2 * asp);
        q.y = 0.5 - k.posY / 2;
        q.w = k.sclX / asp;
        q.h = k.sclY;
        // the rotation's sense is taken as for Event Pan/Crop (clockwise for a positive angle; unconfirmed here)
        q.rotation = k.rotZ * deg;
        q.curve = keyCurveFromVegas(k.type);
        q.smoothness = k.smoothness * 100.0;
        q.z = k.posZ * H / (2 * W);
        q.d = k.depth * 20.0;
        q.rotX = k.rotX * deg;
        q.rotY = k.rotY * deg;
        q.offX = k.orgX / (2 * asp);
        q.offY = -k.orgY * H / (2 * W);
        q.offZ = k.orgZ * H / (2 * W);
        // the orientation is a quaternion of the script's angles composed X, Y, Z: back to the angles
        {
            const double x = k.quat[0], y = k.quat[1], z = k.quat[2], w = k.quat[3];
            const double r02 = 2 * (x * z + y * w), r12 = 2 * (y * z - x * w), r22 = 1 - 2 * (x * x + y * y);
            const double r01 = 2 * (x * y - z * w), r00 = 1 - 2 * (y * y + z * z);
            const double oy = std::asin(std::max(-1.0, std::min(1.0, r02)));
            q.oriY = oy * deg;
            q.oriX = std::atan2(-r12, r22) * deg;
            q.oriZ = std::atan2(-r01, r00) * deg;
            // (the angles of a turn about one axis come out as exact as the quaternion allows)
            for (double* a : {&q.oriX, &q.oriY, &q.oriZ}) if (std::fabs(*a) < 1e-9) *a = 0;
        }
    return q;
}


// A track motion key that leaves the picture where it is (VEGAS writes one when only a shadow or glow is set).
static bool isRestingMotion(const MotionKey& k)
{
    return std::fabs(k.x - 0.5) < 1e-9 && std::fabs(k.y - 0.5) < 1e-9 && std::fabs(k.w - 1) < 1e-9 && std::fabs(k.h - 1) < 1e-9 &&
           std::fabs(k.rotation) < 1e-9 && !k.has3d() && k.smoothness == 0 && k.curve == 0;
}

// The frame aspect a track motion record was made for, else the project's (W x H).
static double motionAspect(const TrackMotionRecord& r, double W, double H)
{
    return (std::isfinite(r.aspect) && r.aspect > 0.01 && r.aspect < 100) ? r.aspect : W / H;
}

// The effects on the whole track: OpenFX plug-ins, VEGAS managed effects Reno has stand-ins for, and the sound effects.
// Others are kept in the file where they were.
void Importer::readTrackFx(const Chunk& tc, Track& t, const std::string& label, VegTrackOrigin& to)
{
    if (const Chunk* fx = childList(tc, G::TrackFx)) {
        for (const auto& k : fx->kids) {
            OfxRecord r;
            FxSlot managed;
            if (!k.isList && k.id == G::Fx && readOfxRecord(k.data, 0, r)) {
                t.fx.push_back(fxSlotOf(r, label, &k, 0, false));
            } else if (managedFxSlotOf(k, label, managed)) {
                t.fx.push_back(managed);
            } else {
                VegHidden hd;
                hd.before = t.fx.size();
                hd.chunks.push_back(&k);
                to.hiddenFx.push_back(hd);
                if (!noteSkippedManagedFx(k, label))
                    notes_.add(label + ": a video effect that is not an OpenFX plug-in is not used by Reno (it is kept in the file).");
            }
        }
    }
    if (const Chunk* fx = childList(tc, G::TrackAudioFx)) {
        if (const Chunk* d = childChunk(*fx, G::TrackAudioFx)) audioChain(d, 0, label, t.fx);
    }
}

// The track's motion (2D and 3D), shadow and glow, in Reno's units.
void Importer::readTrackMotionList(const Chunk& tc, Track& t, const std::string& label)
{
    if (const Chunk* tm = childList(tc, G::TrackMotionList)) {
        for (const Chunk* m : childChunks(*tm, G::TrackMotion)) {
            TrackMotionRecord r;
            if (!readTrackMotion(m->data, r)) notes_.add(label + ": its track motion could not be read (it is kept in the file).");
            else if (!r.untouched) {
                // VEGAS's units (see TrackMotionRecord) to Reno's: the frame's middle as a fraction of the frame (y down),
                // sizes as fractions of the frame, degrees. The position unit is half the frame height.
                const double W = std::max(1, p_->settings.width), H = std::max(1, p_->settings.height);
                const double asp = motionAspect(r, W, H);
                for (const TrackMotionKey& k : r.motion) t.motion.push_back(motionKeyOf(k, W, H, asp));
                // VEGAS's one untouched key (when only the shadow or glow was set) is no motion
                if (t.motion.size() == 1 && isRestingMotion(t.motion[0])) t.motion.clear();
                auto glowKeys = [&](const std::vector<TrackMotionKey>& from, std::vector<GlowKey>& to) {
                    for (const TrackMotionKey& k : from) {
                        GlowKey g;
                        g.time = std::max(0.0, k.time);
                        g.x = k.posX / (2 * asp);
                        g.y = -k.posY / 2;
                        g.w = k.sclX;
                        g.h = k.sclY;
                        g.blur = k.blur;
                        g.intensity = k.intensity;
                        auto ch = [&](int c) { return (uint32_t)std::lround(std::max(0.0f, std::min(1.0f, std::isfinite(k.colour[c]) ? k.colour[c] : 0.0f)) * 255.0f); };
                        g.color = (ch(3) << 24) | (ch(2) << 16) | (ch(1) << 8) | ch(0);
                        g.curve = keyCurveFromVegas(k.type);
                        to.push_back(g);
                    }
                };
                if (r.flags & 0x08) glowKeys(r.shadow, t.shadow2d);
                if (r.flags & 0x10) glowKeys(r.glow, t.glow2d);
            }
        }
    }
}

// Parent Motion (where a compositing parent's finished group goes): a list b12f5229 after the track motion list, holding one
// track motion chunk of the same layout (coverage run 20261105, tracks_video_parentchild: the keys of the script's ParentMotion).
void Importer::readParentMotion(const Chunk& tc, Track& t)
{
    for (const auto& k : tc.kids) {
        if (!k.isList || k.type != "b12f5229-c44c-413e-84bd-a6eff5746218") continue;
        for (const Chunk* m : childChunks(k, G::TrackMotion)) {
            TrackMotionRecord r;
            if (!readTrackMotion(m->data, r) || r.untouched) continue;
            const double W = std::max(1, p_->settings.width), H = std::max(1, p_->settings.height);
            const double asp = motionAspect(r, W, H);
            for (const TrackMotionKey& mk : r.motion) t.parentMotion.push_back(motionKeyOf(mk, W, H, asp));
            if (t.parentMotion.size() == 1 && isRestingMotion(t.parentMotion[0])) t.parentMotion.clear();
        }
    }
}

// The track's events; those Reno cannot read are kept in the file in their places.
void Importer::readTrackEvents(const Chunk& tc, Track& t, const std::string& label, VegTrackOrigin& to)
{
    if (const Chunk* evs = childList(tc, G::Events)) {
        for (const auto& ev : evs->kids) {
            Event e;
            if (ev.isList && ev.type == G::Event && readEvent(ev, t, label, e)) {
                t.events.push_back(e);
            } else {
                VegHidden hd;
                hd.before = t.events.size();
                hd.chunks.push_back(&ev);
                to.hiddenEvents.push_back(hd);
            }
        }
    }
}

void Importer::readTrack(const Chunk& tc, int index)
{
    const Chunk* hc = childChunk(tc, G::TrackHeader);
    if (!hc) {
        notes_.add(strfmt("Track %d could not be read; it is kept in the file but not shown.", index + 1));
        return;
    }
    const Bytes& h = hc->data;
    Track t;
    t.id = p_->newId();
    auto to = origin<VegTrackOrigin>(&tc);
    const uint32_t kind = h.u32(0x0c);
    t.type = kind == 2 ? TrackType::Audio : TrackType::Video;
    if (kind != 2 && kind != 3) notes_.add(strfmt("Track %d is of a kind Reno does not know (%u); it was read as a video track.", index + 1, kind));
    to->vegId = h.u32(0x1c);
    po_->maxTrackId = std::max(po_->maxTrackId, to->vegId);
    const uint32_t nameAt = h.u32(0);
    t.name = h.text(nameAt, h.u32(0x10));
    const uint32_t flags = h.u32(4);
    t.mute = (flags & TrackMute) != 0;
    t.solo = (flags & TrackSolo) != 0;
    t.minimized = (flags & TrackMinimized) != 0;
    const bool automation = (flags & TrackAutomation) != 0;
    t.color = trackColorOfIndex(h.u32(0x14));
    const uint32_t height = h.u32(0x08);
    const uint32_t defaultHeight = t.type == TrackType::Video ? 64 : 68;
    if (height != defaultHeight && height >= 16 && height <= 2000) t.height = (int)height;
    const std::string label = t.name.empty() ? strfmt("Track %d", index + 1) : strfmt("Track %d ('%s')", index + 1, t.name.c_str());

    // an envelope of this kind exists: the automation value is then the envelope's
    bool hasEnvelope[3] = {false, false, false};     // volume or composite level, pan, mute
    if (const Chunk* env = childList(tc, G::TrackEnvelopes)) {
        for (const Chunk* pc : childChunks(*env, G::EnvelopePoints)) {
            EnvelopeRecord er;
            if (!readEnvelope(pc->data, er) || er.points.empty()) continue;
            if (er.type == EnvVolume || er.type == EnvComposite) hasEnvelope[0] = true;
            if (er.type == EnvPan) hasEnvelope[1] = true;
            if (er.type == EnvMute) hasEnvelope[2] = true;
        }
    }
    if (t.type == TrackType::Audio) {
        t.panType = "add";     // VEGAS's default if the settings are missing
        readAudioTrackSettings(tc, t, label, automation, hasEnvelope);
    } else {
        readVideoTrackSettings(tc, t, label, automation, hasEnvelope);
    }

    readTrackFx(tc, t, label, *to);
    readTrackMotionList(tc, t, label);
    readParentMotion(tc, t);
    if (const Chunk* env = childList(tc, G::TrackEnvelopes)) readEnvelopeList(*env, true, t.type, label, t.envelopes, to->hiddenEnvelopes, &t.fx);
    for (const auto& k : tc.kids) {
        if (!k.isList && k.id == G::TrackHeader) continue;
        if (k.isList && (k.type == G::Events || k.type == G::AudioTrackList || k.type == G::VideoTrackList || k.type == G::TrackFx ||
                         k.type == G::TrackAudioFx || k.type == G::TrackMotionList || k.type == G::TrackEnvelopes))
            continue;
        if (k.isList && (k.type == G::TrackExtraA || k.type == G::TrackExtraB) && k.kids.empty()) continue;
        unknown("tracks");
    }

    readTrackEvents(tc, t, label, *to);
    t.sortEvents();
    to->asRead = t;
    to->asRead.events.clear();
    to->asRead.fx.clear();
    to->asRead.envelopes.clear();
    t.veg = to;
    p_->tracks.push_back(t);
}

// ---------------------------------------------------------------- events
// Event chunk (offsets from its start):
//   0x00 u32 0x60 (where the name starts)   0x04 flags: byte 0 bit 0x02 mute (reading), 0x10 selected; byte 1 bit
//        0x01 loop (set on every sample event), 0x02 normalise (audio, certain: matches the stored gain exactly).
//   0x08 u32 2 audio, 3 video   0x10 i64 start   0x18 i64 length   0x20 u32 active take
//   0x24 u32 name bytes   0x28 f64 playback rate   0x40 u64 event id (groups refer to it; top four bits repeat the
//        kind, 2 or 3)   0x48 u32 1 if in a group   then the name, then 0x48 bytes:
//   u32 0x48, u32, u32, f32 gain (opacity) at +0x0c, f32 normalising gain at +0x10, fade-in curve at +0x14 and
//   fade-out curve at +0x18 (CurveType: 1 linear, 2 fast, -2 slow, 4 smooth, -4 sharp; VEGAS defaults fast/slow
//   for sound, smooth/smooth for frames), reciprocal curves of the fades at +0x1c and +0x20 (Fade.ReciprocalCurve:
//   the other side of a cross-fade; default -2 and 2 for sound, 4 and 4 for frames), i64 fade-in length at +0x28,
//   i64 fade-out length at +0x30; then 0x10 bytes starting with u32 0x10.
// The sound event's record: channel mapping, time stretch method, pitch and formant.
void Importer::readEventAudioSettings(const Chunk& ev, Event& e, const std::string& where)
{
if (const Chunk* al = childList(ev, G::AudioEventList)) {
    if (const Chunk* ad = childChunk(*al, G::AudioEventData)) {
        // u32 0x28, u32 at 0x04 channel mapping (ChannelRemapping: 1000 none, 1001 left off, 1002 right off,
        // 1003/1004 a side muted, 1005 mono, 1006 swapped), u32 flags at 0x08, u32 at 0x0c Classic preset (0 = A01
        // .. 18 = A19), u32 at 0x10 elastique mode (0 Pro, 1 Efficient, 2 Soloist (Monophonic), 3 Soloist (Speech)),
        // f64 at 0x14 pitch change in semitones, f64 at 0x1c formant shift, u32 at 0x24 1 if the event has audio
        // effects. Flags: 0x01 invert phase, 0x02 lock to stretch (pitch follows rate), bits 2-3 method (0 Classic,
        // 1 ACID-style, 2 elastique, 3 None), 0x10 preserve formants. Read from vegas180.exe's Audio Event
        // Properties and scripting; matches the samples. VEGAS 18 defaults: elastique, Classic A03, Efficient.
        const Bytes& a = ad->data;
        const uint32_t channels = a.u32(0x04);
        switch (channels) {
        case 1000: e.channels = "both"; break;
        case 1001: e.channels = "right"; break;     // left off: right on both sides
        case 1002: e.channels = "left"; break;
        case 1003: e.channels = "muteleft"; break;
        case 1004: e.channels = "muteright"; break;
        case 1005: e.channels = "combine"; break;
        case 1006: e.channels = "swap"; break;
        default:
            notes_.count("%d sound event(s) have one side muted in VEGAS (Reno has no such setting; it plays both)",
                         where + (channels == 1003 ? ": left muted" : channels == 1004 ? ": right muted" : strfmt(": mapping %u", channels)));
            break;
        }
        const uint32_t flags = a.u32(0x08);
        e.invertPhase = (flags & 0x01) != 0;
        const bool lock = (flags & 0x02) != 0;
        const uint32_t method = (flags >> 2) & 3;
        e.preserveFormants = (flags & 0x10) != 0;
        const uint32_t classic = a.u32(0x0c), elastique = a.u32(0x10);
        e.stretchMethod = method == 3 ? "none" : (method == 2 ? "elastique" : "classic");
        if (method == 1)
            notes_.count("%d sound event(s) use VEGAS's ACID-style stretching (for ACIDized loops); Reno uses Classic for them",
                         where);
        if (method == 2) {
            static const char* const modes[] = {"pro", "efficient", "soloist-mono", "soloist-speech"};
            if (elastique < 4) e.stretchMode = modes[elastique];
            else notes_.count("%d sound event(s) have an élastique mode Reno does not know; Reno uses Pro", where);
        } else if (method != 3) {
            if (classic < 19) e.stretchMode = strfmt("A%02u", classic + 1);
            else {
                e.stretchMode = "A03";
                notes_.count("%d sound event(s) have a Classic stretch preset Reno does not know; Reno uses A03", where);
            }
        }
        const double pitch = a.f64(0x14);
        if (method != 3 && !lock && std::isfinite(pitch)) {
            // the pitch is kept when the rate changes
            e.keepPitch = true;
            e.pitch = std::max(-48.0, std::min(48.0, pitch));
        }
        const double formant = a.f64(0x1c);
        if (std::isfinite(formant)) e.formant = std::max(-24.0, std::min(24.0, formant));
    }
}
}

// The transition of a video event (what plays over the overlap with the event before it) is a record after the first 0x80 bytes
// of the VideoEventData chunk (found in VEGAS Pro 18's coverage projects; 128 bytes when there is none): an OpenFX record, as
// for effects, or for VEGAS's older transitions a record of the older kind (u32 size, class GUID at +4, as in veglegacyfx.h) of
// which Reno knows only the class: it is shown as Reno's own transition of that name with its default settings, and the
// record is written back as it was while the transition is not changed.
void Importer::readEventTransition(const Chunk& vd, Event& e, const std::string& where)
{
    static const struct { const char* guid; const char* plugin; } older[] = {
        {"c843a552-aaeb-411c-8481-cf4c52a336e3", "com.vegascreativesoftware:3dblinds"},
        {"f4d82040-d40f-44ad-bb24-6aab29dc21f1", "com.vegascreativesoftware:3dcascade"},
        {"573fa5a1-f314-4539-9bb1-77fa92d4e7cd", "com.vegascreativesoftware:3dflyinout"},
        {"e38b2b4a-ba99-4f67-b533-1997297850dd", "com.vegascreativesoftware:3dshuffle"},
        {"e97a8331-d099-440d-aac5-9b7cc856ae9f", "com.vegascreativesoftware:gradientwipe"},
        {"5856937c-3603-4378-83cf-889423820f39", "com.vegascreativesoftware:portals"},
        {"bf8ef6da-4ed5-4f9c-b549-919490f88b2f", "com.vegascreativesoftware:venetianblinds"}};
    const Bytes& d = vd.data;
    if (d.size() < 0x80 + 0x20) return;
    OfxRecord r;
    if (isOfxRecordAt(d, 0x80) && readOfxRecord(d, 0x80, r)) {
        e.transition = fxSlotOf(r, where + ", its transition", &vd, 0x80, false);
        e.transition.beforePanCrop = false;
        return;
    }
    const std::string g = d.guid(0x84);
    for (const auto& t : older) {
        if (g != t.guid) continue;
        FxSlot s;
        s.id = p_->newId();
        s.kind = "ofx";
        s.pluginId = t.plugin;
        s.state = Json::object();
        s.state["plugin"] = s.pluginId;
        s.state["params"] = Json::object();
        auto o = origin<VegFxOrigin>(&vd);
        o->entryOff = 0x80;
        o->entryLen = d.size() - 0x80;
        o->asRead = s;
        s.veg = o;
        e.transition = s;
        notes_.count("%d transition(s) are VEGAS's older kind: Reno knows which one, but starts it with its default settings (the file keeps them)", where);
        return;
    }
    notes_.add(where + ": its transition is not one Reno knows; it is kept in the file but not used.");
}

// Video event effects: the media's own first, then the event's; flags from the record.
void Importer::readEventVideoFx(const Chunk& ev, Event& e, VegEventOrigin& eoRef, const MediaRec& m, const std::string& where)
{
    VegEventOrigin* eo = &eoRef;
// the media's effects come first: VEGAS applies them before the event's
size_t mediaFxCount = 0;
{
    // in the order of the media chunk: the OpenFX ones and the older kinds (managed, DirectX-era) that Reno has a stand-in for
    std::vector<std::pair<size_t, const std::pair<OfxRecord, size_t>*>> order;
    for (const auto& r : m.mediaFx) order.push_back({r.second, &r});
    for (size_t off : m.otherFx) order.push_back({off, nullptr});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& it : order) {
        if (it.second) {
            e.fx.push_back(fxSlotOf(it.second->first, where, m.chunk, it.second->second, true));
            ++mediaFxCount;
        } else {
            FxSlot other;
            if (managedFxSlotAt(*m.chunk, it.first, where, other, true)) { e.fx.push_back(other); ++mediaFxCount; }
        }
    }
}
if (mediaFxCount > 0 && mediaFxUsed_.insert(m.vegId).second)
    notes_.add(strfmt("Media '%s' has %zu effect%s of its own (Media FX); Reno has none, so they were put at the start of the "
                      "effects of every event that uses it (saved back as the media's).",
                      windowsFileName(m.name.empty() ? m.path : m.name).c_str(), mediaFxCount, mediaFxCount == 1 ? "" : "s"));
// u32 0x80, u32 flags at 0x04 (bits found by changing one switch of an event, Settings Matrix run 20261028):
// 0x001 Maintain Aspect Ratio (set in every sample; the matrix event with it off has it clear), 0x080 Force Resample,
// 0x100 Reduce Interlace Flicker, 0x200 Resample (smart: the project's resample mode applies). Disable Resample has
// 0x200 and 0x080 clear (57 events in the samples; VEGAS's export says Disable for them). Optical flow resampling was
// not tried. f64 at 0x78: the undersample rate (1.0 in every sample; 0.5 when set to 0.5). u32 at 0x68: last-shown
// effect (-1 none).
if (const Chunk* vl = childList(ev, G::VideoEventList))
    if (const Chunk* vd = childChunk(*vl, G::VideoEventData)) {
        const uint32_t flags = vd->data.u32(4);
        e.resample = (flags & 0x200) ? "project" : (flags & 0x80) ? "force" : "off";
        e.keepAspect = (flags & 0x1) != 0;
        e.reduceFlicker = (flags & 0x100) != 0;
        if (vd->data.size() >= 0x80) {
            const double us = vd->data.f64(0x78);
            if (std::isfinite(us) && us > 0 && us <= 1.0) e.undersample = us;
        }
        readEventTransition(*vd, e, where);
    }
if (const Chunk* fx = childList(ev, G::EventFx)) {
    for (const auto& f : fx->kids) {
        OfxRecord r;
        if (!f.isList && f.id == G::Fx && readOfxRecord(f.data, 0, r)) {
            e.fx.push_back(fxSlotOf(r, where, &f, 0, false));
            continue;
        }
        FxSlot managed;
        if (managedFxSlotOf(f, where, managed)) {
            e.fx.push_back(managed);
            continue;
        }
        VegHidden hd;
        hd.before = e.fx.size();
        hd.chunks.push_back(&f);
        eo->hiddenFx.push_back(hd);
        // Older DirectX-style video plug-ins have their own record: u32 0x38, the plug-in GUID, ..., u32 preset name
        // bytes at 0x34, preset name at 0x38. Their settings mean nothing to an OpenFX host.
        const Bytes& data = f.data;
        if (noteSkippedManagedFx(f, where)) {
            // (reported)
        } else if (!f.isList && data.u32(0) == 0x38 && data.size() > 0x38) {
            const std::string preset = data.text(0x38, data.u32(0x34));
            notes_.count("%d video effect(s) are not OpenFX plug-ins (VEGAS's older kind); Reno cannot use them, but keeps them in the file",
                         where + ": preset '" + preset + "', plug-in {" + data.guid(4) + "}");
        } else {
            notes_.add(where + ": a video effect that could not be read is kept in the file but not used.");
        }
    }
}
}

// Pan/crop keyframes, as fractions of the media size.
void Importer::readEventPanCrop(const Chunk& ev, Event& e, const MediaRec& m, const std::string& where)
{
if (const Chunk* pc = childChunk(ev, G::PanCrop)) {
    PanCropRecord r;
    if (!readPanCrop(pc->data, r) || r.keys.empty()) {
        notes_.add(where + ": its pan/crop could not be read (the file keeps it).");
    } else if (r.mediaW <= 0 || r.mediaH <= 0 || r.mediaW > 65536 || r.mediaH > 65536) {
        notes_.add(where + ": its pan/crop is not used by Reno (the media's picture size is not in the project).");
    } else {
        const double mw = r.mediaW, mh = r.mediaH;
        if (m.renoId && !mediaSize_.count(m.renoId)) mediaSize_[m.renoId] = {r.mediaW, r.mediaH};
        // Untouched pan/crop is one key: the whole frame, widened or heightened to the project's shape, centred.
        // Reno shows that without pan/crop.
        const double outAspect = (double)p_->settings.width / std::max(1, p_->settings.height) * p_->settings.pixelAspect;
        const PanCropRecord::Key& k0 = r.keys[0];
        const bool centred = std::fabs(k0.cx - mw / 2) < 0.01 && std::fabs(k0.cy - mh / 2) < 0.01;
        // a negative width or height is a flipped box (Flip Horizontal / Vertical), never untouched
        const bool fitted = k0.w > 0 && k0.h > 0 &&
                            ((std::fabs(k0.w - mw) < 0.01 && k0.h >= mh - 0.01) || (std::fabs(k0.h - mh) < 0.01 && k0.w >= mw - 0.01));
        const bool shaped = k0.h > 0 && std::fabs(k0.w / k0.h - outAspect) < 0.01;
        const bool untouched = r.keys.size() == 1 && centred && fitted && shaped && std::fabs(k0.rot) < 1e-9;
        if (!untouched) {
            for (const auto& k : r.keys) {
                if (!std::isfinite(k.w) || !std::isfinite(k.h) || !std::isfinite(k.cx) || !std::isfinite(k.cy) || !std::isfinite(k.rot) ||
                    std::fabs(k.w) < 1e-9 || std::fabs(k.h) < 1e-9)
                    continue;
                PanCropKey pk;
                pk.time = std::max(0.0, k.time);
                pk.cx = k.cx / mw;
                pk.cy = k.cy / mh;
                // A negative size is a flip (Flip Horizontal / Vertical); Reno keeps the sign too.
                pk.w = k.w / mw;
                pk.h = k.h / mh;
                // The angle is in radians and turns the Pan/Crop window clockwise (on the screen, y down) for a positive value,
                // as Reno's does, in degrees. (Checked against VEGAS's own pictures of a 0.5 rad key whose corners the
                // scripting API reports: the window's top edge runs down to the right.)
                pk.rotation = k.rot * 180.0 / kPi;
                // VEGAS key types: 0 linear, 1 hold, 2 slow, 3 fast, 4 smooth, 5 sharp (Reno's KeyCurve)
                pk.curve = keyCurveFromVegas((int)k.type);
                // Smoothness: 0..1 in the file, 0..100 in Reno
                if (std::isfinite(k.smooth)) pk.smoothness = std::max(0.0, std::min(100.0, k.smooth * 100.0));
                // rotation centre, relative to the box's centre
                if (std::isfinite(k.rcx) && std::isfinite(k.rcy) && std::fabs(k.rcx - k.cx) < 1e7 && std::fabs(k.rcy - k.cy) < 1e7) {
                    pk.pivotX = (k.rcx - k.cx) / mw;
                    pk.pivotY = (k.rcy - k.cy) / mh;
                    if (std::fabs(pk.pivotX) < 1e-9) pk.pivotX = 0;
                    if (std::fabs(pk.pivotY) < 1e-9) pk.pivotY = 0;
                }
                e.panCrop.push_back(pk);
            }
        }
        if (r.mask) notes_.add(where + ": its pan/crop mask is not used by Reno (the file keeps it).");
    }
}
}

// The event's level, its normalising gain and its fades: the record after the name, at 's'.
void Importer::readEventLevels(const Bytes& d, size_t s, bool normalize, TrackType type, const std::string& where, Event& e)
{
    const float gain = d.f32(s + 0x0c), norm = d.f32(s + 0x10);
    double g = (std::isfinite(gain) && gain >= 0 && gain < 64) ? gain : 1.0;
    // Edit > Switches > Normalize: VEGAS stores the measured normalising gain apart from the event's level
    if (normalize && type == TrackType::Audio && std::isfinite(norm) && norm > 0 && norm < 1000) {
        e.normalize = true;
        e.normalizeGain = norm;
    }
    e.gain = g;
    const double length = e.length;
    const double fin = (double)d.i64(s + 0x28) / 1e7, fout = (double)d.i64(s + 0x30) / 1e7;
    if (fin > 0 && fin <= length) e.fadeIn = fin;
    if (fout > 0 && fout <= length) e.fadeOut = fout;
    bool k1 = true, k2 = true;
    e.fadeInCurve = curveFromVegas(d.i32(s + 0x14), &k1);
    e.fadeOutCurve = curveFromVegas(d.i32(s + 0x18), &k2);
    // +0x1c is FadeIn.ReciprocalCurve: how the previous event fades out under this one (VEGAS keeps both curves
    // of a cross-fade on the later event). +0x20 FadeOut.ReciprocalCurve is not used.
    bool k3 = true;
    const int recip = curveFromVegas(d.i32(s + 0x1c), &k3);
    if (k3) e.fadeInRecipCurve = recip;
    if ((!k1 && e.fadeIn > 0) || (!k2 && e.fadeOut > 0)) notes_.count("%d fade(s) have a curve Reno does not know; they are straight lines in Reno", where);
}

// A take: the media record it plays and the offset in it (timeline time, from the record's start: a subclip's start,
// or for a reversed subclip its end), and its name. False if its media record was not read.
bool Importer::takeOf(const Bytes& tk, double rate, double length, Take& out)
{
    auto it = media_.find(tk.u32(0x24));
    if (it == media_.end()) return false;
    const MediaRec& m = it->second;
    double off = (double)tk.i64(0x10) / 1e7 * rate;
    if (!(off >= 0) || off > 1e7) off = 0;
    out.mediaId = m.renoId;
    out.reverse = false;
    if (m.kind == VegMediaRecord::Subclip) {
        if (m.subReversed) {
            out.offset = std::max(0.0, mirrorEndOf(m) - off - length * rate);
            out.reverse = true;
        } else {
            out.offset = m.subStart + off;
        }
    } else {
        out.offset = off;
    }
    const uint32_t nameAt = tk.u32(0), nameLen = tk.u32(0x20);
    if (nameAt >= 0x28 && nameLen < 0x10000) out.name = tk.text(nameAt, nameLen);
    return true;
}

// Take: u32 0x2a, ..., i64 offset at 0x10, i64 length at 0x18, u32 name bytes at 0x20, u32 media id at 0x24, name
// at 0x2a. The offset is in timeline time: the media position is offset x rate (a slowed event in the samples
// starts 2283.6 s "in" to a 628 s video: 570.9 s at rate 0.25).
bool Importer::readEvent(const Chunk& ev, Track& track, const std::string& label, Event& e)
{
    const Chunk* dc = childChunk(ev, G::EventData);
    if (!dc || dc->data.size() < 0x60) {
        notes_.count("%d event(s) could not be read; they are kept in the file but not shown", label);
        return false;
    }
    const Bytes& d = dc->data;
    e = Event();
    e.id = p_->newId();
    const double start = (double)d.i64(0x10) / 1e7, length = (double)d.i64(0x18) / 1e7;
    double rate = d.f64(0x28);
    const std::string where = label + ", event at " + secs(start);
    if (!(start >= 0) || !(length > 0) || start > 1e7 || length > 1e7) {
        notes_.count("%d event(s) with an impossible position or length are kept in the file but not shown", where);
        return false;
    }
    if (!std::isfinite(rate) || rate <= 0 || rate > 1000) {
        notes_.add(where + ": its playback rate could not be read; Reno uses 1.");
        rate = 1.0;
    }
    auto eo = origin<VegEventOrigin>(&ev);
    eo->vegId = d.u64(0x40);
    e.start = start;
    e.length = length;
    // i64 at 0x30: the snap offset (Coverage run 20261105: 0.25 s there reads 2500000)
    {
        const double snap = (double)d.i64(0x30) / 1e7;
        if (snap > 0 && snap <= length) e.snapOffset = snap;
    }
    const uint32_t nameAt = d.u32(0), nameLen = d.u32(0x24);
    if (nameAt == 0x60 && nameLen < 0x10000) e.name = d.text(nameAt, nameLen);
    const size_t s = (size_t)nameAt + nameLen;
    const uint8_t f0 = d.u8(4), f1 = d.u8(5);
    e.mute = (f0 & 0x02) != 0;
    e.locked = (f0 & 0x08) != 0;
    e.loop = (f1 & 0x01) != 0;
    const bool normalize = (f1 & 0x02) != 0;
    if (d.has(s, 0x38)) readEventLevels(d, s, normalize, track.type, where, e);

    // active take
    const std::vector<const Chunk*> takes = childChunks(ev, G::Take);
    if (takes.empty()) {
        notes_.count("%d empty event(s) (no media) are kept in the file but not shown", where);
        return false;
    }
    const uint32_t active = std::min<uint32_t>(d.u32(0x20), (uint32_t)takes.size() - 1);
    eo->activeTake = active;
    if (rate <= 0 || !std::isfinite(rate)) rate = 1.0;
    for (size_t i = 0; i < takes.size(); ++i) {
        Take tk;
        const bool ok = takeOf(takes[i]->data, rate, length, tk);
        eo->takesRead.push_back(tk);
        eo->takesOk.push_back(ok);
        if (i != active && ok) e.takes.push_back(tk);
    }
    const Bytes& tk = takes[active]->data;
    const uint32_t mid = tk.u32(0x24);
    auto mit = media_.find(mid);
    if (mit == media_.end()) {
        notes_.count("%d event(s) use media Reno could not read; they are kept in the file but not shown", where);
        return false;
    }
    eo->takeMedia = mid;
    const MediaRec& m = mit->second;
    Take act;
    takeOf(tk, rate, length, act);
    e.mediaId = act.mediaId;
    e.rate = rate;
    e.offset = act.offset;
    e.reverse = act.reverse;
    // the active take's place among all of them (VEGAS's order), counting the ones Reno could read
    for (size_t i = 0; i < active && i < eo->takesOk.size(); ++i)
        if (eo->takesOk[i]) ++e.activeTake;

    if (const Chunk* env = childList(ev, G::EventEnvelopes)) readEnvelopeList(*env, false, track.type, where, e.envelopes, eo->hiddenEnvelopes);

    if (track.type == TrackType::Audio) {
        readEventAudioSettings(ev, e, where);
        if (const Chunk* fx = childList(ev, G::EventAudioFx))
            if (const Chunk* fd = childChunk(*fx, G::EventAudioFx)) audioChain(fd, 0, where, e.fx);
    } else {
        readEventVideoFx(ev, e, *eo, m, where);
        readEventPanCrop(ev, e, m, where);
        // The media's own effects work on the picture as the event shows it: VEGAS's pictures of an event without Pan/Crop (a 320x180
        // picture in a 640x360 project, with effects on the media) are what the same effects give on the enlarged frame, not the effects'
        // result at the picture's own size enlarged afterwards (Reno's pre-pan/crop stage). With Pan/Crop they stay before it.
        if (e.panCrop.empty() && e.keepAspect)
            for (FxSlot& s : e.fx)
                if (const auto* fo = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK))
                    if (fo->mediaFx) s.beforePanCrop = false;
    }

    // anything else in the event (kept in the file as it is)
    for (const auto& k : ev.kids) {
        if (!k.isList && (k.id == G::EventData || k.id == G::Take || k.id == G::PanCrop)) continue;
        if (k.isList && (k.type == G::VideoEventList || k.type == G::AudioEventList || k.type == G::EventFx || k.type == G::EventAudioFx ||
                         k.type == G::EventEnvelopes))
            continue;
        unknown("events");
    }

    vegEventToReno_[eo->vegId] = e.id;
    eo->asRead = e;
    eo->asRead.fx.clear();
    eo->asRead.envelopes.clear();
    e.veg = eo;
    return true;
}

}  // namespace vegread
}  // namespace reno
