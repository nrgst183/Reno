// Reads VEGAS Pro projects (.veg) into a Reno project. See vegformat.h for the file layout.
// This file: file layout, media, buses, groups, markers and the public entry points.
// vegas_tracks.cpp: envelopes, tracks and events. Importer is declared in vegas_internal.h.
//
// Everything read keeps a link to its source bytes (vegorigin.h), so saving as .veg (vegwrite.cpp) writes back
// what VEGAS wrote for anything unchanged, including parts Reno cannot use.
// Field meanings worked out from samples but not confirmed are marked as such. tests/vegas/compare.cpp checks the
// reader against VEGAS's own output (tools/vegas/Export to Reno.cs).
#include "vegas.h"
#include "vegas_internal.h"
#include "vegvst3side.h"
#include "../audiofx/audioplugin.h"
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>

namespace reno {

namespace {

// Media file properties, read from the file itself (defined below).
bool mediaFileInfo(const std::string& path, MediaInfo& info);

// Maximum nesting depth when reading projects used as media.
thread_local int t_nestDepth = 0;
thread_local std::set<std::string>* t_nestOpen = nullptr;

}  // namespace

namespace vegread {

// The video output's envelopes (fade to colour, mute, motion blur length, supersampling) are a list of their own at the top
// of the file: an envelope header and an envelope points chunk each, as on a track (Coverage run 20261105).
static const char* const kVideoBusEnvelopes = "f0ca0912-3506-496f-bdbf-7807976210d2";


// "META:\SubClip\{C:\a\b.mp4} (2)[1184656300][7400444][0]": file, subclip start and length (100 ns), reversed flag.
bool parseSubclip(const std::string& s, std::string& file, double& start, double& len, bool& reversed)
{
    const std::string pre = "META:\\SubClip\\{";
    if (s.size() <= pre.size() || s.compare(0, pre.size(), pre) != 0) return false;
    long long nums[3] = {0, 0, 0};
    size_t end = s.size();
    for (int i = 2; i >= 0; --i) {
        if (end == 0 || s[end - 1] != ']') return false;
        const size_t open = s.rfind('[', end - 1);
        if (open == std::string::npos) return false;
        const std::string n = s.substr(open + 1, end - 1 - open - 1);
        if (n.empty() || n.size() > 18 || n.find_first_not_of("0123456789") != std::string::npos) return false;
        nums[i] = std::stoll(n);
        end = open;
    }
    const size_t close = s.rfind("} (", end);
    if (close == std::string::npos || close < pre.size()) return false;
    file = s.substr(pre.size(), close - pre.size());
    start = (double)nums[0] / 1e7;
    len = (double)nums[1] / 1e7;
    reversed = nums[2] != 0;
    return !file.empty();
}


bool Importer::run(Project& p, std::string& error)
{
    p_ = &p;
    po_ = origin<VegProjectOrigin>(&tree_.root);
    const Chunk& root = tree_.root;
    if (tree_.damaged)
        notes_.add("The file is damaged (" + tree_.problem + "); everything before the damage was read.");

    const Chunk* header = childChunk(root, G::Header);
    if (!header) {
        error = "the project's properties are missing: this is not a VEGAS project, or it is badly damaged";
        return false;
    }
    readHeader(*header);
    if (const Chunk* s = childList(root, G::Summary))
        if (const Chunk* d = childChunk(*s, G::SummaryData)) readSummary(*d);
    if (const Chunk* m = childList(root, G::MediaList)) readMedia(*m);
    if (const Chunk* b = childList(root, G::Buses)) {
        readBuses(*b);
        if (const Chunk* m = childChunk(*b, G::Mixer)) readMixer(*m);
    }
    if (const Chunk* vo = childList(root, G::VideoOutFx)) readVideoOutFx(*vo);
    for (const auto& k : root.kids) {
        if (k.isList && k.type == kVideoBusEnvelopes) {
            std::vector<VegHidden> hidden;
            readEnvelopeList(k, true, TrackType::Video, "The video output", p_->videoEnvelopes, hidden);
            po_->videoEnvelopes = p_->videoEnvelopes;
        }
    }
    if (const Chunk* t = childList(root, G::Tracks)) {
        int i = 0;
        for (const Chunk* tr : childLists(*t, G::Track)) readTrack(*tr, i++);
        for (const auto& k : t->kids)
            if (!(k.isList && k.type == G::Track)) unknown("the track list");
    }
    if (const Chunk* g = childList(root, G::Groups)) readGroups(*g);
    if (const Chunk* m = childList(root, G::Markers)) readMarkers(*m);
    if (const Chunk* m = childList(root, G::CommandMarkers)) readMarkers(*m, 1);
    if (const Chunk* m = childList(root, G::CdMarkers)) readMarkers(*m, 2);
    finishMedia();
    nameUnnamedAudioFx();

    // Top-level parts that are only VEGAS window/view state are kept and written back silently.
    static const char* const known[] = {G::Header, G::MediaList, G::Tracks, G::Groups, G::Markers, G::Summary, G::Buses, G::VideoOutFx,
                                        G::ViewList, G::Timelines, G::WindowState, G::EmptyListA, G::EmptyChunkA};
    for (const auto& k : root.kids) {
        bool ok = false;
        for (const char* g : known) ok = ok || k.is(g);
        if (k.isList && (k.type == G::CommandMarkers || k.type == G::CdMarkers)) ok = true;     // read above
        if (k.isList && k.type == kVideoBusEnvelopes) ok = true;                                 // read above
        if (k.isList && k.type == G::MarkersB) {
            ok = true;
            const size_t n = childChunks(k, G::Marker).size();
            if (n)
                notes_.add(strfmt("%zu marker(s) of an unknown kind %s not shown; the file keeps %s.", n, n == 1 ? "is" : "are",
                                  n == 1 ? "it" : "them"));
        }
        if (!ok) unknown("the project");
    }
    for (const auto& u : unknownChunks_)
        notes_.add(strfmt("%d part%s of %s %s not understood; %s kept in the file but not used.", u.second, u.second == 1 ? "" : "s",
                          u.first.c_str(), u.second == 1 ? "was" : "were", u.second == 1 ? "it is" : "they are"));
    po_->settings = p.settings;
    po_->masterVolume = p.masterVolume;
    p.veg = po_;
    return true;
}

// ---------------------------------------------------------------- project properties
// Header chunk: u32 0x90 (where the paths start), u32 sample rate at 0x0c, f64 frame rate at 0x10, f64 tempo at
// 0x18, u16 beats per measure at 0x28; u32 at 0x3c, 0x40, 0x44: byte sizes (with the 0) of the first three texts;
// u64 at 0x48: last event id handed out. Then four 0-terminated texts (project path, documents folder, VEGAS
// settings folder, one more) and three 0x358-byte video settings blocks: u32 size, u32 width, u32 height, ...,
// f64 frame rate at 0x38, f64 pixel aspect at 0x40. The first block is the project's; the other two are unknown.
// Render quality (+0x1c) and deinterlacing (+0xf4) are in the first block. Field order, pixel format, motion blur
// are in there somewhere, but no sample shows where, so Reno's defaults are kept. The header word at 0x08 is the ruler's
// time format (VEGAS's own numbering, not the scripting API's; checked against VEGAS's export of 20 projects: 1 is Time,
// 3 Time & Frames, 7 SMPTE 30; 0 and 8 also occur and are not known, so they leave Reno's Time & Frames).
void Importer::readHeader(const Chunk& c)
{
    const Bytes& b = c.data;
    ProjectSettings& s = p_->settings;
    const uint32_t rate = b.u32(0x0c);
    if (rate >= 8000 && rate <= 768000) s.sampleRate = (int)rate;
    const double fps = b.f64(0x10);
    if (std::isfinite(fps) && fps >= 1.0 && fps <= 1000.0) {
        // NTSC rates are stored as 29.97002997..; snap to 30000/1001 etc.
        const double ntsc[] = {24000.0 / 1001, 30000.0 / 1001, 60000.0 / 1001, 120000.0 / 1001};
        bool done = false;
        for (double n : ntsc)
            if (std::fabs(fps - n) < 1e-4) { s.fpsNum = (int)std::lround(n * 1001); s.fpsDen = 1001; done = true; }
        if (!done) {
            if (std::fabs(fps - std::round(fps)) < 1e-6) { s.fpsNum = (int)std::lround(fps); s.fpsDen = 1; }
            else { s.fpsNum = (int)std::lround(fps * 1000); s.fpsDen = 1000; }
        }
    }
    const double tempo = b.f64(0x18);
    if (std::isfinite(tempo) && tempo >= 10 && tempo <= 1000) s.tempo = tempo;
    s.rulerFormat = rulerFormatOfHeaderWord(b.u32(0x08));
    // i64 at 0x20: the ruler's start time, stored negated (a ruler that starts at 0.5 s holds -5000000)
    const int64_t rulerStart = -b.i64(0x20);
    if (rulerStart > -(int64_t)36000000000000LL && rulerStart < (int64_t)36000000000000LL) s.rulerStart = (double)rulerStart / 1e7;
    // u16 at 0x28: the note that gets one beat (4 = quarter note), u16 at 0x2a: beats per measure (3/8 holds 8 and 3)
    const uint16_t note = b.u16(0x28), bpm = b.u16(0x2a);
    if (bpm >= 1 && bpm <= 32) s.beatsPerMeasure = bpm;
    if (note >= 1 && note <= 64) s.noteGetsBeat = note;
    // u32 at 0x70: the audio CD's first track number; u32 at 0x74 and 0x78: the low-pass filter on the LFE (5.1) switch
    // and its cutoff in Hz
    const uint32_t firstTrack = b.u32(0x70);
    if (firstTrack >= 1 && firstTrack <= 99) s.cdFirstTrack = (int)firstTrack;
    if (b.u32(0x74) <= 1) s.lfeLowpass = b.u32(0x74) == 1;
    const uint32_t lfeCutoff = b.u32(0x78);
    if (lfeCutoff >= 10 && lfeCutoff <= 1000) s.lfeCutoff = lfeCutoff;
    po_->lastEventId = b.u64(0x48);

    size_t at = b.u32(0);
    if (at < 0x40 || at > b.size()) return;
    const std::string projectPath = b.textZ(at, &at);
    po_->headerPath = projectPath;
    for (int i = 0; i < 2; ++i) b.textZ(at, &at);
    s.cdUpc = b.textZ(at, &at);      // the fourth text is the audio CD's UPC / media catalog number
    const size_t sep = projectPath.find_last_of("\\/");
    if (sep != std::string::npos) originalDir_ = projectPath.substr(0, sep + 1);
    const uint32_t blk = b.u32(at);
    const uint32_t w = b.u32(at + 4), h = b.u32(at + 8);
    if (blk >= 0x50 && w >= 16 && w <= 16384 && h >= 16 && h <= 16384) {
        s.width = (int)w;
        s.height = (int)h;
        const double par = b.f64(at + 0x40);
        if (std::isfinite(par) && par > 0.1 && par < 10) s.pixelAspect = par;
        // Render quality at +0x1c (3 Good, 4 Best in every sample; Draft = 1 and Preview = 2 are assumed) and
        // deinterlace at +0xf4 (0 none, 1 blend, 2 interpolate assumed). Matches VEGAS's export of 7 samples.
        if (b.has(at, 0xf8)) {
            const uint32_t q = b.u32(at + 0x1c), d = b.u32(at + 0xf4);
            static const char* const quality[] = {"", "draft", "preview", "good", "best"};
            if (q >= 1 && q <= 4) s.renderQuality = quality[q];
            if (d <= 2) s.deinterlace = d == 1 ? "blend" : d == 2 ? "interpolate" : "none";
        }
        // HDR mode at +0x348 (0 off, 1 HDR10, 2 HLG) and 360 output at +0x344 (0 off, 1 on): found by saving projects with each
        // set (Coverage run 2, 20261105); older files have the words at 0
        if (b.has(at, 0x34c)) {
            const uint32_t hdr = b.u32(at + 0x348), t360 = b.u32(at + 0x344);
            if (hdr <= 2) s.hdrMode = hdr == 1 ? "hdr10" : hdr == 2 ? "hlg" : "off";
            if (t360 <= 1) s.output360 = t360 == 1;
        }
        // The video output's fade-to-colour colours: top at +0xa0 and bottom at +0xb0, four floats each in the order blue, green,
        // red, alpha (white and black unless set; Coverage run 20261105)
        if (b.has(at, 0xc0)) {
            auto colour = [&](size_t off) {
                auto ch = [&](size_t o) {
                    const float f = b.f32(at + off + o);
                    return (uint32_t)std::lround(std::max(0.0f, std::min(1.0f, std::isfinite(f) ? f : 0.0f)) * 255.0f);
                };
                return (ch(12) << 24) | (ch(8) << 16) | (ch(4) << 8) | ch(0);
            };
            p_->videoFadeTop = colour(0xa0);
            p_->videoFadeBottom = colour(0xb0);
            po_->videoFadeTop = p_->videoFadeTop;
            po_->videoFadeBottom = p_->videoFadeBottom;
        }
        // Found by changing one setting at a time (Coverage run 20261105): +0x34 field order (0 progressive, 1 upper,
        // 2 lower), +0xf0 motion blur type (VEGAS's MotionBlurType: 0 Gaussian, 1 Pyramid, 2 Box, 3-5 the asymmetric
        // ones), +0x0c pixel format (0x1c 8-bit, 0x20 32-bit floating point, video levels), +0x130 output rotation
        // (0 none, 1 clockwise, 2 upside down, 3 counterclockwise).
        if (b.has(at, 0x134)) {
            const uint32_t fo = b.u32(at + 0x34), mb = b.u32(at + 0xf0), pf = b.u32(at + 0x0c), rot = b.u32(at + 0x130);
            if (fo <= 2) s.fieldOrder = fo == 1 ? "upper" : fo == 2 ? "lower" : "none";
            static const char* const blur[] = {"gaussian", "pyramid", "box", "asymgaussian", "asympyramid", "asymbox"};
            if (mb < 6) s.motionBlur = blur[mb];
            if (pf == 0x20) { s.pixelFormat = "32bit"; s.pixelRange = "video"; }
            else if (pf == 0x1c) s.pixelFormat = "8bit";
            if (rot <= 3) s.outputRotation = (int)rot * 90;
        }
    } else {
        notes_.add("The project's picture size could not be read; 1920x1080 is assumed.");
    }
}

// INFO-style entries: u32 8, u32 how many, then four-character code, u32 bytes, UTF-16 text.
void Importer::readSummary(const Chunk& c)
{
    const Bytes& b = c.data;
    const uint32_t n = b.u32(4);
    size_t at = 8;
    for (uint32_t i = 0; i < n && i < 64 && b.has(at, 8); ++i) {
        const uint32_t cc = b.be32(at), len = b.u32(at + 4);
        if (!b.has(at + 8, len)) break;
        const std::string text = b.text(at + 8, len);
        switch (cc) {
        case 0x494e414du: p_->settings.title = text; break;      // INAM
        case 0x49415254u: p_->settings.artist = text; break;     // IART
        case 0x49454e47u: p_->settings.engineer = text; break;   // IENG
        case 0x49434f50u: p_->settings.copyright = text; break;  // ICOP
        case 0x49434d54u: p_->settings.comments = text; break;   // ICMT
        default: break;
        }
        at += 8 + (size_t)len;
    }
}

// The mixer chunk holds the audio device name and, at 0x6e4 and 0x6e8, a sample rate and bit depth. The rate
// differs from the project's in two samples, so the bit depth is only used where they agree. Unconfirmed.
void Importer::readMixer(const Chunk& c)
{
    const Bytes& b = c.data;
    const uint32_t rate = b.u32(0x6e4), bits = b.u32(0x6e8);
    // The project's sample rate is the mixer's: the header's word at 0x0c stays 48000 when a script or the dialog
    // sets another rate (props_audio_* of the Coverage run: 8000..96000 all kept 48000 there, and 192000 only agreed
    // because the project's mixer had it), and two of the samples have 44100 here and 48000 there.
    if (rate >= 8000 && rate <= 768000) p_->settings.sampleRate = (int)rate;
    if (bits == 16 || bits == 24 || bits == 32) p_->settings.audioBits = (int)bits;
    // u32 at 0x0c and 0x10: the master bus mode (1 and 2: stereo; 2 and 6: 5.1 surround)
    if (b.u32(0x0c) == 2 && b.u32(0x10) == 6) p_->settings.audioChannels = 6;
}

// ---------------------------------------------------------------- media
// Media chunk: u32 0x80 (where the path starts), u32 path bytes at 0x24, u32 byte sizes of two more texts at 0x28
// and 0x2c (both 2, i.e. empty, or both 0 in the samples), u32 stream count at 0x38, u32 media id (what takes refer
// to) at 0x3c, u32 name bytes at 0x40. After the path come the two texts, the name, then records that start with
// their size: 0x90 for a stream (u32 type at +4: 3 video, 2 sound), 0x224 and 0xb0 for a generator (the 0xb0 one
// holds width and height at +0x0c and +0x10), followed for a generator by u32 1 and its OpenFX record. Further
// OpenFX records are taken as the media's own effects (Media FX); no sample has any, so that is unconfirmed.
// The record has no length or size of the media: VEGAS reads the file.
void Importer::readMedia(const Chunk& list)
{
    for (const Chunk* c : childChunks(list, G::Media)) {
        const Bytes& b = c->data;
        MediaRec m;
        m.chunk = c;
        const uint32_t hs = b.u32(0);
        const uint32_t pathLen = b.u32(0x24), nameLen = b.u32(0x40);
        const uint32_t moreTexts = std::min<uint32_t>(b.u32(0x28), 0x10000) + std::min<uint32_t>(b.u32(0x2c), 0x10000);
        m.vegId = b.u32(0x3c);
        po_->maxMediaId = std::max(po_->maxMediaId, m.vegId);
        VegMediaRecord vr;
        vr.chunk = c;
        vr.vegId = m.vegId;
        if (hs < 0x40 || !b.has(hs, pathLen)) {
            notes_.count("%d media entr(y/ies) could not be read (they are kept in the file)");
            vr.kind = VegMediaRecord::Other;
            po_->media.push_back(vr);
            continue;
        }
        const std::string raw = b.text(hs, pathLen);
        size_t at = hs + (size_t)pathLen + moreTexts;
        if (b.has(at, nameLen)) m.name = b.text(at, nameLen);
        at += nameLen;
        const size_t recordsStart = at;
        for (int i = 0; i < 64 && b.has(at, 8); ++i) {
            const uint32_t sz = b.u32(at);
            if (sz < 8 || !b.has(at, sz)) break;
            if (sz == 0x90) {
                const uint32_t type = b.u32(at + 4);
                if (type == 3) m.video = true;
                else if (type == 2) m.audio = true;
            } else if (sz == 0xc) {
                const double f = b.f64(at + 4);
                if (std::isfinite(f) && f > 0 && f <= 1000) m.seqFps = f;
            } else if (sz == 0xb0) {
                m.genW = (int)b.u32(at + 0x0c);
                m.genH = (int)b.u32(at + 0x10);
            } else if (sz == 0x224) {
                m.generatorName = b.textZ(at + 0x1c);
            }
            at += sz;
        }
        // OpenFX records follow the stream records; searched for, not counted
        std::vector<std::pair<OfxRecord, size_t>> fx;
        {
            static const uint8_t ofxGuid[16] = {0x41, 0x1a, 0xfc, 0xa2, 0xf6, 0x01, 0x0e, 0x46, 0xae, 0x5a, 0x2b, 0xe4, 0xaf, 0x57, 0xb9, 0x2e};
            size_t f = b.find(ofxGuid, 16, (size_t)hs + pathLen);
            while (f != std::string::npos && fx.size() < 1000) {
                OfxRecord r;
                if (f >= 4 && readOfxRecord(b, f - 4, r) && r.end > f) {
                    const size_t next = r.end;
                    fx.push_back({std::move(r), f - 4});
                    f = b.find(ofxGuid, 16, next);
                } else {
                    f = b.find(ofxGuid, 16, f + 1);
                }
            }
        }
        // Media effects of the older kinds (VEGAS's managed CoreGraphics effects such as Rays, and the DirectX-era ones) are records of
        // their own behind the stream records (u32 0x38, the class GUID): found by position, one after the other
        {
            size_t q = recordsStart;
            while (q + 0x38 <= b.size() && m.otherFx.size() < 1000) {
                bool inOfx = false;
                for (const auto& o : fx) inOfx = inOfx || (q >= o.second && q < o.first.end);
                if (inOfx) { q += 1; continue; }
                if (b.u32(q) == 0x38 && (isManagedFxAt(b, q) || isLegacyFxAt(b, q))) {
                    m.otherFx.push_back(q);
                    q += 0x38;
                } else {
                    q += 1;
                }
            }
        }
        double ss = 0, sl = 0;
        bool rev = false;
        std::string parent;
        if (parseSubclip(raw, parent, ss, sl, rev)) {
            m.kind = VegMediaRecord::Subclip;
            m.path = parent;
            m.subStart = ss;
            m.subLen = sl;
            m.subReversed = rev;
            m.mediaFx = fx;
        } else if (raw.compare(0, 22, "META:\\Video Generator\\") == 0) {
            m.kind = VegMediaRecord::Generated;
            m.video = true;
            if (!fx.empty()) {
                m.generator = fx.front().first;
                m.generatorOff = fx.front().second;
                m.generatorOk = true;
                m.mediaFx.assign(fx.begin() + 1, fx.end());
            }
        } else if (raw.compare(0, 22, "META:\\Image Sequence\\{") == 0 && raw.find("} (") != std::string::npos) {
            // the first picture is in the braces; the numbers after it are the frame count and the digits of the numbering
            m.kind = VegMediaRecord::File;
            m.sequence = true;
            m.path = raw.substr(22, raw.rfind("} (") - 22);
            m.video = true;
            // VEGAS names it "first - last" (no name is stored): the numbers are [count][digits] at the end of the text
            {
                long long nums[2] = {0, 0};
                size_t end = raw.size();
                bool ok = true;
                for (int i = 1; i >= 0 && ok; --i) {
                    const size_t open = end > 0 && raw[end - 1] == ']' ? raw.rfind('[', end - 1) : std::string::npos;
                    ok = open != std::string::npos && end - open - 2 > 0 && end - open - 2 < 10;
                    if (ok) { nums[i] = std::atoll(raw.substr(open + 1, end - open - 2).c_str()); end = open; }
                }
                const std::string file = windowsFileName(m.path);
                const size_t dot = file.find_last_of('.');
                const std::string stem = dot == std::string::npos ? file : file.substr(0, dot);
                const size_t digits = (size_t)nums[1];
                if (ok && nums[0] >= 2 && digits > 0 && digits < stem.size() + 1 && digits < 10 && m.name.empty()) {
                    const std::string head = stem.substr(0, stem.size() - digits);
                    const long first = std::atol(stem.c_str() + stem.size() - digits);
                    char last[16];
                    std::snprintf(last, sizeof last, "%0*ld", (int)digits, first + (long)nums[0] - 1);
                    m.name = stem + " - " + head + last;
                }
            }
            m.mediaFx = fx;
        } else if (raw.compare(0, 5, "META:") == 0) {
            m.kind = VegMediaRecord::Other;
            m.path = raw;
        } else {
            m.kind = VegMediaRecord::File;
            m.path = raw;
            m.mediaFx = fx;
        }
        vr.kind = m.kind;
        vr.path = m.kind == VegMediaRecord::Other ? raw : m.path;
        vr.subStart = m.subStart;
        vr.subLen = m.subLen;
        vr.subReversed = m.subReversed;
        // The pre-OpenFX generators (DirectX plug-ins, named by class GUID in the path) have no OpenFX record; the two that
        // Reno has a built-in of are taken with that built-in's default settings (their own settings block is not
        // decoded). The record stays as it is in the file unless the generator is changed.
        const char* legacyPlugin = nullptr;
        if (m.kind == VegMediaRecord::Generated && !m.generatorOk && raw.size() > 22) {
            static const struct { const char* guid; const char* plugin; } legacyGen[] = {
                {"{02FD3B60-986B-11D4-9F6F-00485481BD31}", "com.vegascreativesoftware:creditroll"},
                {"{8B5DEABC-53BD-49F6-9803-367478307A59}", "com.vegascreativesoftware:colorgradient"},
            };
            for (const auto& g : legacyGen)
                if (raw.compare(22, std::strlen(g.guid), g.guid) == 0) legacyPlugin = g.plugin;
            if (legacyPlugin)
                notes_.count("%d generator(s) are VEGAS's older DirectX generators (Credit Roll, Color Gradient); their settings are not read, Reno uses the built-in's defaults",
                             m.generatorName.empty() ? raw : m.generatorName);
        }
        if (m.kind == VegMediaRecord::Generated && !m.generatorOk && !legacyPlugin) {
            notes_.add("Generated media '" + (m.name.empty() ? raw : m.name) + "': the generator's settings could not be read; it is kept "
                       "in the file but not shown.");
            vr.kind = VegMediaRecord::Other;
            po_->media.push_back(vr);
            continue;
        }
        if (m.kind == VegMediaRecord::Other) {
            notes_.add("Media '" + raw + "' is of an unknown kind (not a file, subclip or video generator); it is kept in the file, "
                       "but events using it are not shown.");
            po_->media.push_back(vr);
            continue;
        }
        if (m.kind == VegMediaRecord::Generated) {
            MediaItem mi;
            mi.id = p_->newId();
            mi.generated = true;
            mi.name = !m.name.empty() ? m.name : (!m.generatorName.empty() ? m.generatorName : m.generator.pluginId);
            if (legacyPlugin) {
                mi.generator.id = p_->newId();
                mi.generator.kind = "ofx";
                mi.generator.pluginId = legacyPlugin;
            } else {
                mi.generator = fxSlotOf(m.generator, "Generated media '" + mi.name + "'", c, m.generatorOff, false);
            }
            mi.generator.name = m.generatorName;
            mi.info.hasVideo = true;
            if (m.genW >= 1 && m.genW <= 16384 && m.genH >= 1 && m.genH <= 16384) {
                mi.info.width = m.genW;
                mi.info.height = m.genH;
            }
            auto o = origin<VegMediaOrigin>(c);
            o->vegId = m.vegId;
            o->asRead = mi;
            mi.veg = o;
            m.renoId = mi.id;
            p_->media.push_back(mi);
        } else {
            // A subclip becomes its whole file: events using it get the subclip start added to their offset
            // (and are reversed if the subclip plays backwards).
            const std::string display = m.kind == VegMediaRecord::File ? m.name : std::string();
            m.renoId = mediaItemForFile(m.path, display, m.sequence);
            MediaItem* mi = p_->findMedia(m.renoId);
            if (mi && m.sequence && mi->sequenceFps <= 0) mi->sequenceFps = m.seqFps > 0 ? m.seqFps : p_->settings.fps();     // (the rate the sequence was opened at; the project's when the record has none)
            if (mi) {
                mi->info.hasVideo = mi->info.hasVideo || m.video;
                mi->info.hasAudio = mi->info.hasAudio || m.audio;
                if (m.kind == VegMediaRecord::File) {
                    if (!m.name.empty()) mi->name = windowsFileName(m.name);
                    // (two records for one file become one item: it keeps the first record, so its place in the pool is that record's)
                    const auto* had = vegOriginAs<VegMediaOrigin>(mi->veg, VegOrigin::MediaK);
                    if (!had || !had->vegId) {
                        auto o = origin<VegMediaOrigin>(c);
                        o->vegId = m.vegId;
                        mi->veg = o;           // asRead copy is made in finishMedia
                    }
                }
            }
            if (m.kind == VegMediaRecord::Subclip) {
                double& len = mediaMinLength_[m.renoId];
                len = std::max(len, m.subStart + m.subLen);
            }
        }
        vr.renoId = m.renoId;
        po_->media.push_back(vr);
        media_[m.vegId] = m;
    }
}

uint64_t Importer::mediaItemForFile(const std::string& path, const std::string& name, bool sequence)
{
    const std::string key = normPath(path) + (sequence ? "\x01sequence" : "");
    auto it = fileMedia_.find(key);
    if (it != fileMedia_.end()) return it->second;
    MediaItem mi;
    mi.id = p_->newId();
    mi.path = path;
    mi.name = windowsFileName(!name.empty() ? name : path);
    mi.info.path = path;
    // item used only by subclips: no record of its own
    auto o = origin<VegMediaOrigin>(nullptr);
    mi.veg = o;
    fileMedia_[key] = mi.id;
    p_->media.push_back(mi);
    return mi.id;
}

// End point used to mirror a reversed subclip (see reversedClipEnd): its end, cut back to the end of the file's
// picture. The file is found as in finishMedia; if it is missing the subclip's end is used.
double Importer::mirrorEndOf(const MediaRec& m)
{
    const double plain = m.subStart + m.subLen;
    auto it = mirrorEnd_.find(m.vegId);
    if (it != mirrorEnd_.end()) return it->second;
    double end = plain;
    std::string found = fileExists(m.path) ? m.path : std::string();
    if (found.empty()) {
        const std::string here = pathDir(vegPath_).empty() ? std::string(".") : pathDir(vegPath_);      // (a bare file name is in the current folder)
        const std::string origDir = normPath(originalDir_);
        const std::string np = normPath(m.path);
        std::string rel;
        if (!origDir.empty() && np.size() > origDir.size() && np.compare(0, origDir.size(), origDir) == 0) rel = m.path.substr(origDir.size());
        if (!rel.empty() && !here.empty()) {
            std::string r = rel;
#ifndef _WIN32
            for (auto& c : r) if (c == '\\') c = '/';
#endif
            const std::string cand = pathJoin(here, r);
            if (fileExists(cand)) found = cand;
        }
        if (found.empty() && !here.empty()) {
            const std::string base = windowsFileName(m.path);
            const std::string cand = pathJoin(here, base);
            if (!base.empty() && fileExists(cand)) found = cand;
        }
    }
    if (!found.empty() && !isStillImage(found) && !endsWithNoCase(found, ".veg")) {
        MediaInfo info;
        if (mediaFileInfo(found, info)) end = reversedClipEnd(m.subStart, m.subLen, &info);
    }
    mirrorEnd_[m.vegId] = end;
    return end;
}

// Files not at the stored path are looked for as if the project folder had moved (as VEGAS does), then marked
// offline. Other .veg files used as media are read for frame size, frame rate and length.
void Importer::finishMedia()
{
    const std::string here = pathDir(vegPath_).empty() ? std::string(".") : pathDir(vegPath_);      // (a bare file name is in the current folder)
    const std::string origDir = normPath(originalDir_);
    for (auto& m : p_->media) {
        if (m.generated) continue;
        if (!fileExists(m.path)) {
            const std::string np = normPath(m.path);
            std::string rel;
            if (!origDir.empty() && np.size() > origDir.size() && np.compare(0, origDir.size(), origDir) == 0)
                rel = m.path.substr(origDir.size());
            std::string found;
            if (!rel.empty() && !here.empty()) {
                std::string r = rel;
#ifndef _WIN32
                for (auto& c : r) if (c == '\\') c = '/';
#endif
                const std::string cand = pathJoin(here, r);
                if (fileExists(cand)) found = cand;
            }
            if (found.empty() && !here.empty()) {
                const std::string base = windowsFileName(m.path);
                const std::string cand = pathJoin(here, base);
                if (!base.empty() && fileExists(cand)) found = cand;
            }
            if (!found.empty()) {
                m.path = found;
                m.info.path = found;
            } else {
                m.offline = true;
            }
        }
        // Length, frame rate and sample rate come from the file (the .veg does not store them); events need them to
        // loop, clamp, snap to the media's frame grid and draw thumbnails and waveforms.
        if (!m.offline && !isStillImage(m.path) && !endsWithNoCase(m.path, ".veg")) {
            MediaInfo info;
            if (mediaFileInfo(m.path, info)) {
                info.path = m.path;
                m.info = info;
                mediaMinLength_.erase(m.id);            // the file's own length replaces the subclip estimate
            }
        }
        if (isStillImage(m.path)) {
            m.info.isStill = true;
            m.info.hasVideo = true;
            m.info.hasAudio = false;
        }
        auto sz = mediaSize_.find(m.id);
        if (sz != mediaSize_.end()) {
            m.info.width = sz->second.first;
            m.info.height = sz->second.second;
        }
        auto ln = mediaMinLength_.find(m.id);
        if (ln != mediaMinLength_.end() && !m.info.isStill) m.info.duration = ln->second;
        if (endsWithNoCase(m.path, ".veg") && !m.offline) {
            MediaInfo info;
            std::string err;
            if (vegasProjectMediaInfo(m.path, info, &err)) {
                info.path = m.path;
                m.info = info;
            } else {
                notes_.add("Media '" + m.name + "' is another VEGAS project that could not be read (" + err + ").");
            }
        }
        // snapshot to compare with when saving
        if (auto* o = vegOriginAs<VegMediaOrigin>(m.veg, VegOrigin::MediaK)) {
            auto c = std::make_shared<VegMediaOrigin>(*o);
            c->asRead = m;
            c->asRead.veg.reset();
            m.veg = c;
        }
    }
    int offline = 0;
    for (const auto& m : p_->media) offline += m.offline ? 1 : 0;
    if (offline > 0)
        notes_.add(strfmt("%d media file%s could not be found (kept with their paths and marked offline).", offline, offline == 1 ? "" : "s"));
}

// ---------------------------------------------------------------- effects
FxSlot Importer::fxSlotOf(const OfxRecord& r, const std::string& where, const Chunk* chunk, size_t off, bool mediaFx)
{
    FxSlot out;
    out.id = p_->newId();
    out.kind = "ofx";
    out.pluginId = r.pluginId;
    out.state = r.state;
    out.preset = r.preset;
    out.bypass = r.bypass();
    out.beforePanCrop = r.beforePanCrop() || mediaFx;      // media effects apply to the media itself
    if (r.unreadParams > 0) {
        std::string names;
        for (size_t i = 0; i < r.unreadNames.size() && i < 4; ++i) names += (i ? ", " : "") + r.unreadNames[i];
        notes_.add(strfmt("%s: %d setting%s of '%s' could not be read; Reno uses %s default%s (the file keeps %s)%s.", where.c_str(),
                          r.unreadParams, r.unreadParams == 1 ? "" : "s", r.pluginId.c_str(), r.unreadParams == 1 ? "its" : "their",
                          r.unreadParams == 1 ? "" : "s", r.unreadParams == 1 ? "it" : "them",
                          names.empty() ? "" : (" (" + names + ")").c_str()));
    }
    if (chunk) {
        auto o = origin<VegFxOrigin>(chunk);
        o->entryOff = off;
        o->entryLen = r.end > off ? r.end - off : 0;
        o->mediaFx = mediaFx;
        o->asRead = out;
        out.veg = o;
    }
    return out;
}

// Reports a managed-effect chunk as unused (it is kept in the file); returns false if the chunk is not one.
bool Importer::noteSkippedManagedFx(const Chunk& chunk, const std::string& where)
{
    ManagedFxRecord r;
    if (chunk.isList || chunk.id != G::Fx || !readManagedFx(chunk.data, 0, r)) return false;
    notes_.count("%d video effect(s) are VEGAS's own managed effects (CoreGraphics) that Reno cannot read; they are kept in the file but not used",
                 where + ": " + r.root + ", preset '" + r.preset + "', class {" + r.classGuid + "}");
    return true;
}

bool Importer::managedFxSlotOf(const Chunk& chunk, const std::string& where, FxSlot& out)
{
    if (chunk.isList || chunk.id != G::Fx) return false;
    return managedFxSlotAt(chunk, 0, where, out, false);
}

// The record at 'off' of the chunk's data (a media chunk holds its own effects behind the stream records).
bool Importer::managedFxSlotAt(const Chunk& chunk, size_t off, const std::string& where, FxSlot& out, bool mediaFx)
{
    ManagedFxRecord r;
    LegacyFxRecord lr;
    Json state;
    bool bypass = false;
    std::vector<std::string> problems;
    std::string label, plugin;
    size_t recordEnd = 0;
    if (readManagedFx(chunk.data, off, r)) {
        plugin = managedPluginId(r);
        if (plugin.empty() || !managedFxState(r, p_->settings.fps(), state, bypass, problems)) return false;
        label = r.root;
        recordEnd = r.end;
    } else if (readLegacyFx(chunk.data, off, lr)) {
        plugin = legacyPluginId(lr);
        if (plugin.empty() || !legacyFxState(lr, p_->settings.fps(), state, problems)) return false;
        label = plugin.substr(plugin.find(':') + 1);
        recordEnd = lr.end;
    } else {
        return false;
    }
    out = FxSlot();
    out.id = p_->newId();
    out.kind = "ofx";
    out.pluginId = plugin;
    out.state = state;
    out.preset = !lr.classGuid.empty() ? lr.preset : r.preset;
    out.bypass = bypass;
    out.beforePanCrop = recordBeforePanCrop(chunk.data, off) || mediaFx;      // (a media effect works on the media itself)
    if (!lr.classGuid.empty()) out.bypass = legacyBypass(lr);
    else if (chunk.data.size() >= off + 0x30 && (chunk.data.u32(off + 0x2c) & 1u)) out.bypass = true;      // managed effects: the record's bypass bit as well as their XML
    if (!problems.empty()) {
        std::string names;
        for (size_t i = 0; i < problems.size() && i < 4; ++i) names += (i ? ", " : "") + problems[i];
        notes_.add(strfmt("%s: %zu setting%s of VEGAS's %s could not be read; Reno uses the effect's default%s (%s).", where.c_str(), problems.size(),
                          problems.size() == 1 ? "" : "s", label.c_str(), problems.size() == 1 ? "" : "s", names.c_str()));
    }
    auto o = origin<VegFxOrigin>(&chunk);
    o->entryOff = off;
    o->entryLen = recordEnd > off ? recordEnd - off : 0;
    o->mediaFx = mediaFx;
    o->asRead = out;
    out.veg = o;
    return true;
}

void Importer::audioChain(const Chunk* chunk, size_t off, const std::string& where, FxChain& into)
{
    const Bytes& data = chunk->data;
    std::vector<AudioPluginRecord> plugs;
    std::string problem;
    if (!readAudioChain(data, off, plugs, &problem) && plugs.empty()) {
        notes_.add(where + ": the audio effects could not be read (" + problem + "); they are kept in the file.");
        return;
    }
    if (!problem.empty()) notes_.add(where + ": some audio effects could not be read (" + problem + ").");
    AudioChainParts parts;
    const bool split = splitAudioChain(data.sub(off, data.size() - off), parts) && parts.entries.size() == plugs.size();
    for (size_t i = 0; i < plugs.size(); ++i) {
        const auto& a = plugs[i];
        FxSlot s;
        s.id = p_->newId();
        s.kind = "audio";
        s.name = a.name;
        s.bypass = a.bypass;
        s.audio.format = a.format;
        s.audio.uid = a.uid;
        s.audio.name = a.name;
        if (a.format == "VST2") {
            // the CLSID as text ("6e744157-...") back to the bytes as stored: the first three fields are little-endian
            std::string hex;
            for (char c : a.clsid) if (c != '-') hex += c;
            if (hex.size() == 32) {
                auto flip = [&](size_t at, size_t bytes) { for (size_t i = 0; i < bytes / 2; ++i) std::swap_ranges(hex.begin() + at + 2 * i, hex.begin() + at + 2 * i + 2, hex.begin() + at + 2 * (bytes - 1 - i)); };
                flip(0, 4);
                flip(8, 2);
                flip(12, 2);
                s.audio.vegasClsid = hex;
            }
        }
        if (!a.state.empty()) s.state = a.state;
        if (!a.problem.empty()) notes_.add(where + ": audio effect '" + a.name + "': " + a.problem + "; it starts with its default settings in Reno.");
        // (VST effects are matched by id at playback, DirectX effects by CLSID)
        if (split) {
            auto o = origin<VegFxOrigin>(chunk);
            o->entryOff = off + parts.entries[i].first;
            o->entryLen = parts.entries[i].second;
            o->asRead = s;
            s.veg = o;
        }
        into.push_back(s);
    }
}

// An Assignable FX chain keeps no names for its plug-ins (VEGAS shows the plug-in's own): take the name another chain of the
// project gives the same plug-in, else the name of Reno's own version of a VEGAS DirectX effect. Only the label changes:
// the slot as read gets the same name, so the writer still sees it unchanged.
void Importer::nameUnnamedAudioFx()
{
    std::map<std::string, std::string> byUid;
    auto learn = [&](const FxChain& c) {
        for (const auto& s : c)
            if (s.kind == "audio" && !s.audio.name.empty() && !s.audio.uid.empty()) byUid.emplace(s.audio.uid, s.audio.name);
    };
    for (const auto& t : p_->tracks) {
        learn(t.fx);
        for (const auto& e : t.events) learn(e.fx);
    }
    for (const auto& b : p_->buses) learn(b.fx);
    learn(p_->audioOut);
    std::vector<AudioPluginDesc> builtins;
    auto fill = [&](FxChain& c) {
        for (auto& s : c) {
            if (s.kind != "audio" || !s.audio.name.empty() || s.audio.uid.empty()) continue;
            std::string name;
            auto it = byUid.find(s.audio.uid);
            if (it != byUid.end()) name = it->second;
            else if (s.audio.format == "DX") {
                const std::string b = builtinUidForDx(s.audio.uid);
                if (!b.empty()) {
                    if (builtins.empty()) builtins = builtinAudioPlugins();
                    for (const auto& d : builtins) if (d.uid == b) name = d.name;
                }
            }
            if (name.empty()) continue;
            s.audio.name = name;
            if (s.name.empty()) s.name = name;
            if (auto* o = const_cast<VegFxOrigin*>(vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK))) {
                o->asRead.audio.name = s.audio.name;
                o->asRead.name = s.name;
            }
        }
    };
    for (auto& b : p_->buses) if (b.assignable) fill(b.fx);
}

// ---------------------------------------------------------------- buses
// Bus chunk: u32 0xc8, u32 kind at 0x0c (2 master, 3 audio bus, 7 video bus), u32 index at 0x14 (1 = Bus A), f64
// level at 0x18 (and 0x20 for the right side); u32 mute at 0x5c, u32 solo at 0x60, u32 pan type at 0x7c (as a track's,
// see panTypeName; the master has no pan type there); at 0xd0 four 0-terminated texts (output device, name,
// description, default name "Bus A"), then the audio effect chain. At 0x98 the audio and video buses hold -2 in every sample, taken to mean
// "to the master"; anything else is reported.
int curveFromVegas(int32_t c, bool* known);     // (vegas_tracks.cpp)

void Importer::readBuses(const Chunk& list)
{
    // The envelopes of the master or of a bus are inside its record
    auto envelopesOf = [&](const Chunk* c, const std::string& label, bool master) {
        Envelopes out;
        std::vector<EnvelopeRecord> recs;
        if (!readBusEnvelopes(c->data, recs)) return out;
        for (const auto& e : recs) {
            Envelope env;
            if (e.type == EnvVolume) env.kind = "volume";
            else if (e.type == EnvPan && !master) env.kind = "pan";
            else if (e.type == EnvMute) env.kind = "mute";
            else {
                notes_.add(strfmt("%s: its %s envelope (%zu point%s) is not used by Reno (it is kept in the file).", label.c_str(),
                                  envelopeName(e.type).c_str(), e.points.size(), e.points.size() == 1 ? "" : "s"));
                continue;
            }
            bool allKnown = true;
            for (const auto& pt : e.points) {
                EnvPoint q;
                q.time = pt.time;
                q.value = pt.value;
                bool known = true;
                q.curve = curveFromVegas(pt.curve, &known);
                allKnown = allKnown && known;
                env.points.push_back(q);
            }
            if (!allKnown) notes_.add(label + ": an envelope has points with a curve Reno does not know; they are straight lines in Reno.");
            std::stable_sort(env.points.begin(), env.points.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
            out.push_back(env);
        }
        return out;
    };
    static const uint8_t chainGuid[16] = {0x01, 0x86, 0x12, 0xb4, 0x2e, 0x70, 0xd1, 0x11, 0x82, 0x53, 0x00, 0x00, 0x92, 0x90, 0xdd, 0x44};
    for (const Chunk* c : childChunks(list, G::Bus)) {
        const Bytes& b = c->data;
        const uint32_t type = b.u32(0x0c);
        const uint32_t index = b.u32(0x14);
        const double level = finiteOr(b.f64(0x18), 1.0);
        size_t at = 0xd0;
        b.textZ(at, &at);
        std::string name = b.has(at, 2) ? b.textZ(at) : std::string();
        const size_t chain = b.find(chainGuid, 16, 0xd0);
        if (type == 2) {
            if (level >= 0 && level < 16) p_->masterVolume = level;
            p_->masterMute = b.u32(busMuteOffset) != 0;
            if (chain != std::string::npos) audioChain(c, chain, "The master bus", p_->audioOut);
            p_->masterEnvelopes = envelopesOf(c, "The master bus", true);
            po_->masterEnvelopes = p_->masterEnvelopes;
        } else if (type == 3) {
            Bus bus;
            bus.id = p_->newId();
            bus.name = name.empty() ? strfmt("Bus %c", (char)('A' + std::min<uint32_t>(index ? index - 1 : 0, 25))) : name;
            bus.level = (level >= 0 && level < 16) ? level : 1.0;
            bus.mute = b.u32(busMuteOffset) != 0;
            bus.solo = b.u32(busSoloOffset) != 0;
            bus.panType = b.u32(busPanTypeOffset) == 0 ? std::string() : panTypeName(b.u32(busPanTypeOffset));     // ("": VEGAS's default, Add Channels)
            if (b.i32(0x98) != -2)
                notes_.add("Bus '" + bus.name + "': where its sound goes could not be read; Reno sends it to the master.");
            if (chain != std::string::npos) audioChain(c, chain, "Bus '" + bus.name + "'", bus.fx);
            bus.envelopes = envelopesOf(c, "Bus '" + bus.name + "'", false);
            auto o = origin<VegBusOrigin>(c);
            o->asRead = bus;
            o->asRead.fx.clear();
            bus.veg = o;
            busByIndex_[index] = bus.id;
            p_->buses.push_back(bus);
        } else if (type == 4) {
            // An Assignable FX chain: the same record as a bus (type 3), named "FX n" by VEGAS (after its number, 0 first)
            Bus bus;
            bus.id = p_->newId();
            bus.assignable = true;
            bus.name = strfmt("FX %u", index + 1);
            bus.level = (level >= 0 && level < 16) ? level : 1.0;
            bus.mute = b.u32(busMuteOffset) != 0;
            bus.solo = b.u32(busSoloOffset) != 0;
            if (chain != std::string::npos) audioChain(c, chain, "Assignable FX " + std::to_string(index + 1), bus.fx);
            auto o = origin<VegBusOrigin>(c);
            o->asRead = bus;
            o->asRead.fx.clear();
            bus.veg = o;
            p_->buses.push_back(bus);
        } else if (type == 7) {
            // video output effects: OpenFX records, as on events
            static const uint8_t ofxGuid[16] = {0x41, 0x1a, 0xfc, 0xa2, 0xf6, 0x01, 0x0e, 0x46, 0xae, 0x5a, 0x2b, 0xe4, 0xaf, 0x57, 0xb9, 0x2e};
            size_t f = b.find(ofxGuid, 16, 0);
            while (f != std::string::npos && p_->videoOut.size() < 1000) {
                OfxRecord r;
                if (f >= 4 && readOfxRecord(b, f - 4, r) && r.end > f) {
                    const size_t next = r.end;
                    p_->videoOut.push_back(fxSlotOf(r, "The video output", c, f - 4, false));
                    f = b.find(ofxGuid, 16, next);
                } else {
                    f = b.find(ofxGuid, 16, f + 1);
                }
            }
        } else {
            unknown("the mixer");
        }
    }
}

// The effects of the video output (Video Output FX): a list of its own between the summary and the buses, one effect
// record (the same as an event's) per chunk. Found in VEGAS Pro 18's own coverage projects; older files hold none.
void Importer::readVideoOutFx(const Chunk& list)
{
    const std::string where = "The video output";
    for (const auto& k : list.kids) {
        OfxRecord r;
        FxSlot managed;
        if (!k.isList && k.id == G::Fx && readOfxRecord(k.data, 0, r)) {
            p_->videoOut.push_back(fxSlotOf(r, where, &k, 0, false));
        } else if (managedFxSlotOf(k, where, managed)) {
            p_->videoOut.push_back(managed);
        } else {
            VegHidden hd;
            hd.before = p_->videoOut.size();
            hd.chunks.push_back(&k);
            po_->hiddenVideoOutFx.push_back(hd);
            if (!noteSkippedManagedFx(k, where))
                notes_.add(where + ": a video effect that is not an OpenFX plug-in is not used by Reno (it is kept in the file).");
        }
    }
}

// ---------------------------------------------------------------- groups and markers
// A group: u32 0x10, u32 (1), u32 how many, u32, then the events' ids (u64 each).
void Importer::readGroups(const Chunk& list)
{
    for (const Chunk* g : childChunks(list, G::Group)) {
        const Bytes& b = g->data;
        const uint32_t n = b.u32(8);
        std::vector<uint64_t> members;
        for (uint32_t i = 0; i < n && i < 100000 && b.has(0x10 + (size_t)i * 8, 8); ++i) {
            auto it = vegEventToReno_.find(b.u64(0x10 + (size_t)i * 8));
            if (it != vegEventToReno_.end()) members.push_back(it->second);
        }
        if (members.size() < 2) continue;
        const uint64_t gid = p_->newId();
        for (uint64_t id : members)
            if (Event* e = p_->findEvent(id)) {
                e->group = gid;
                // the snapshot follows, so grouping alone does not count as a change
                if (auto* eo = vegOriginAs<VegEventOrigin>(e->veg, VegOrigin::EventK)) const_cast<VegEventOrigin*>(eo)->asRead.group = gid;
            }
    }
}

// Marker or region: u32 0x28 (where the name starts), i64 position at 0x08, i64 length at 0x10 (regions), u32
// number at 0x18 (markers and regions are numbered separately), u32 flags at 0x1c (0x01: region), u32 name bytes at
// 0x20, u32 second-text bytes at 0x24, then the two texts. Same record in all four marker lists (vegas180.exe
// _sfmarker::Persist). With a kind the header is 0x30 bytes, kind (four letters) at 0x28; the only one written
// is "cdng", a CD track with ISRC code (u32 and the 12-letter code after the texts). Reno reads its times and name
// and keeps the rest.
// Lists: CommandMarkerCOM uses the list saved as 90020ef5, CDMarkerCOM / CDRegionCOM the one saved as 90020ef7;
// both lists hold markers and regions. A command marker's second text is "<command>:<parameter>", split at the
// first ':' (a TAB before a character makes it part of the command); its name is the marker's comment.
void Importer::readMarkers(const Chunk& list, int which)
{
    for (const Chunk* c : childChunks(list, G::Marker)) {
        const Bytes& b = c->data;
        Project::Marker m;
        m.id = p_->newId();
        m.time = (double)b.i64(8) / 1e7;
        m.length = (double)b.i64(0x10) / 1e7;
        if (!(m.time >= 0) || m.time > 1e7) continue;
        if (!(m.length >= 0) || m.length > 1e7) m.length = 0;
        m.region = (b.u32(0x1c) & 0x01) != 0 || m.length > 0;
        const uint32_t nameAt = b.u32(0), nameBytes = b.u32(0x20);
        m.name = b.text(nameAt, nameBytes);
        if (which == 1) {
            m.kind = "command";
            m.region = false;
            m.length = 0;
            std::string cmd, param;
            splitMarkerCommand(b.text((size_t)nameAt + nameBytes, b.u32(0x24)), cmd, param);
            m.command = cmd;
            m.comment = m.name;     // first text is the command's comment
            m.name = param;
        } else if (which == 2) {
            m.kind = m.region ? "cdtrack" : "cdindex";
        }
        auto o = origin<VegMarkerOrigin>(c);
        o->asRead = m;
        m.veg = o;
        p_->markers.push_back(m);
    }
    for (const auto& k : list.kids)
        if (k.isList || k.id != G::Marker) unknown("the marker list");
}

}  // namespace vegread

using namespace vegread;


// ------------------------------------------------------------------------------------------ public
bool isVegasProject(const std::string& path)
{
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(widen(path).c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    uint8_t buf[40];
    const size_t n = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    return n == sizeof buf && looksLikeVeg(Bytes(buf, n));
}

VegasImportResult importVegasData(const std::string& raw, const std::string& path, Project& out)
{
    VegasImportResult r;
    try {
        if (!looksLikeVeg(Bytes((const uint8_t*)raw.data(), raw.size()))) {
            r.error = "this is not a VEGAS Pro project";
            return r;
        }
        // bytes are shared by everything read from them, for writing back
        auto file = std::make_shared<VegFile>();
        file->bytes = raw;
        file->path = path;
        readChunkTree(Bytes((const uint8_t*)file->bytes.data(), file->bytes.size()), file->tree);
        Project p;
        Importer im(path, file);
        std::string err;
        if (!im.run(p, err)) {
            r.error = err;
            r.notes = im.notes();
            return r;
        }
        r.notes = im.notes();
        out = p;
        r.ok = true;
    } catch (const std::exception& ex) {
        r.ok = false;
        r.error = std::string("the project could not be read (") + ex.what() + ")";
    }
    return r;
}

VegasImportResult importVegasProject(const std::string& path, Project& out)
{
    std::string raw;
    if (!readFile(path, raw)) {
        VegasImportResult r;
        r.error = "the file could not be read";
        return r;
    }
    VegasImportResult r = importVegasData(raw, path, out);
    if (r.ok) readVst3Side(path, out, &r.notes);       // VST3 effects kept beside the .veg (VEGAS cannot store them)
    return r;
}

// Cache of vegasProjectMediaInfo results per project file, valid while size and time are unchanged. Projects that
// nest many others would otherwise re-read each one for every use.
namespace {
struct InfoMemo { int64_t size = -1, time = 0; MediaInfo info; };
std::mutex gInfoMemoMutex;
std::map<std::string, InfoMemo> gInfoMemo;

bool fileStamp(const std::string& path, int64_t& size, int64_t& time)
{
    try {
        const std::filesystem::path fp = std::filesystem::u8path(path);
        std::error_code ec;
        const auto sz = std::filesystem::file_size(fp, ec);
        if (ec) return false;
        const auto t = std::filesystem::last_write_time(fp, ec);
        if (ec) return false;
        size = (int64_t)sz;
        time = (int64_t)t.time_since_epoch().count();
        return true;
    } catch (...) {
        return false;
    }
}
// The same for media files read while opening a project.
struct FileInfoMemo { int64_t size = -1, time = 0; bool ok = false; MediaInfo info; };
std::mutex gFileInfoMutex;
std::map<std::string, FileInfoMemo> gFileInfo;

bool mediaFileInfo(const std::string& path, MediaInfo& info)
{
    int64_t size = -1, time = 0;
    const bool stamped = fileStamp(path, size, time);
    const std::string key = normPath(path);
    if (stamped) {
        std::lock_guard<std::mutex> lk(gFileInfoMutex);
        auto it = gFileInfo.find(key);
        if (it != gFileInfo.end() && it->second.size == size && it->second.time == time) {
            info = it->second.info;
            return it->second.ok;
        }
    }
    bool ok = false;
    try {
        std::string err;
        std::unique_ptr<MediaReader> r = openMedia(path, &err);
        if (r) {
            info = r->info();
            ok = true;
        }
    } catch (...) {
        ok = false;
    }
    if (stamped) {
        std::lock_guard<std::mutex> lk(gFileInfoMutex);
        FileInfoMemo& m = gFileInfo[key];
        m.size = size;
        m.time = time;
        m.ok = ok;
        m.info = ok ? info : MediaInfo();
    }
    return ok;
}

}  // namespace

bool vegasProjectMediaInfo(const std::string& path, MediaInfo& info, std::string* error)
{
    // stops self-nesting and nesting deeper than any real project
    std::set<std::string> openHere;
    const bool outermost = t_nestOpen == nullptr;
    if (outermost) t_nestOpen = &openHere;
    struct Guard {
        bool outer;
        ~Guard()
        {
            --t_nestDepth;
            if (outer) t_nestOpen = nullptr;
        }
    } guard{outermost};
    ++t_nestDepth;
    const std::string key = normPath(path);
    if (t_nestDepth > 8 || t_nestOpen->count(key)) {
        if (error) *error = "it uses itself, or other projects nested too deep";
        return false;
    }
    int64_t size = -1, time = 0;
    const bool stamped = fileStamp(path, size, time);
    if (stamped) {
        std::lock_guard<std::mutex> lk(gInfoMemoMutex);
        auto it = gInfoMemo.find(key);
        if (it != gInfoMemo.end() && it->second.size == size && it->second.time == time) {
            info = it->second.info;
            info.path = path;
            return true;
        }
    }
    t_nestOpen->insert(key);
    Project p;
    const VegasImportResult r = importVegasProject(path, p);
    t_nestOpen->erase(key);
    if (!r.ok) {
        if (error) *error = r.error;
        return false;
    }
    struct Remember {
        bool on;
        const std::string& key;
        int64_t size, time;
        MediaInfo& info;
        ~Remember()
        {
            if (!on) return;
            std::lock_guard<std::mutex> lk(gInfoMemoMutex);
            InfoMemo& m = gInfoMemo[key];
            m.size = size;
            m.time = time;
            m.info = info;
        }
    } remember{stamped, key, size, time, info};
    info = MediaInfo();
    info.path = path;
    info.width = p.settings.width;
    info.height = p.settings.height;
    info.fps = p.settings.fps();
    info.duration = p.length();
    info.sampleRate = p.settings.sampleRate;
    info.channels = 2;
    for (const auto& t : p.tracks) {
        if (t.events.empty()) continue;
        if (t.type == TrackType::Video) info.hasVideo = true;
        else info.hasAudio = true;
    }
    info.videoCodec = info.hasVideo ? "VEGAS project" : "";
    info.audioCodec = info.hasAudio ? "VEGAS project" : "";
    return true;
}

bool loadProjectFileWithNotes(const std::string& path, Project& out, std::string* error, std::vector<std::string>* notes)
{
    if (isVegasProject(path)) {
        const VegasImportResult r = importVegasProject(path, out);
        if (!r.ok) {
            if (error) *error = r.error;
            return false;
        }
        if (notes) *notes = r.notes;
        return true;
    }
    Project p;
    if (!p.load(path, error)) return false;
    out = p;
    if (notes) notes->clear();
    return true;
}

bool loadProjectFile(const std::string& path, Project& out, std::string* error)
{
    return loadProjectFileWithNotes(path, out, error, nullptr);
}

// ---------------------------------------------------------------- older effects, for the model dump
static VegOlderFx olderFxAt(const Bytes& b, size_t off)
{
    VegOlderFx f;
    std::string g = b.guid(off + 4);
    for (char& c : g) c = (char)std::toupper((unsigned char)c);
    f.classGuid = g;
    if (b.has(off, 0x38)) {
        const uint32_t flags = b.u32(off + 0x2c);
        f.bypass = (flags & 1u) != 0;
        f.beforePanCrop = (flags & 0xC0u) != 0;
        const uint32_t n = b.u32(off + 0x34);
        if (n >= 2 && n < 4096 && b.has(off + 0x38, n)) f.preset = b.text(off + 0x38, n);
    }
    return f;
}

bool vegOlderFxOf(const FxSlot& s, VegOlderFx& out)
{
    const auto* o = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK);
    if (!o || !o->chunk || isOfxRecordAt(o->chunk->data, o->entryOff) || !o->chunk->data.has(o->entryOff, 0x20)) return false;
    out = olderFxAt(o->chunk->data, o->entryOff);
    return true;
}

bool vegLegacyGeneratorOf(const MediaItem& m, VegOlderFx& out)
{
    const auto* o = vegOriginAs<VegMediaOrigin>(m.veg, VegOrigin::MediaK);
    if (!m.generated || !o || !o->chunk || m.generator.veg) return false;
    // the record behind the stream records (u32 0x38, the class GUID, the preset name), found as the importer finds media effects
    const Bytes& b = o->chunk->data;
    const char* guid = m.generator.pluginId == "com.vegascreativesoftware:creditroll" ? "02fd3b60-986b-11d4-9f6f-00485481bd31"
                     : m.generator.pluginId == "com.vegascreativesoftware:colorgradient" ? "8b5deabc-53bd-49f6-9803-367478307a59" : nullptr;
    if (!guid) return false;
    for (size_t q = 0x40; q + 0x38 <= b.size(); ++q)
        if (b.u32(q) == 0x38 && b.guid(q + 4) == guid) {
            out = olderFxAt(b, q);
            return true;
        }
    return false;
}

static std::vector<VegOlderFx> hiddenOf(const std::vector<VegHidden>& list)
{
    std::vector<VegOlderFx> out;
    for (const VegHidden& h : list)
        for (const Chunk* c : h.chunks) {
            if (!c || c->isList || !c->data.has(0, 0x20)) continue;
            VegOlderFx f = olderFxAt(c->data, 0);
            f.before = h.before;
            out.push_back(f);
        }
    return out;
}

std::vector<VegOlderFx> vegHiddenFxOf(const Track& t)
{
    const auto* o = vegOriginAs<VegTrackOrigin>(t.veg, VegOrigin::TrackK);
    return o ? hiddenOf(o->hiddenFx) : std::vector<VegOlderFx>();
}

std::vector<VegOlderFx> vegHiddenFxOf(const Event& e)
{
    const auto* o = vegOriginAs<VegEventOrigin>(e.veg, VegOrigin::EventK);
    return o ? hiddenOf(o->hiddenFx) : std::vector<VegOlderFx>();
}

std::vector<VegOlderFx> vegHiddenVideoOutFxOf(const Project& p)
{
    const auto* o = vegOriginAs<VegProjectOrigin>(p.veg, VegOrigin::ProjectK);
    return o ? hiddenOf(o->hiddenVideoOutFx) : std::vector<VegOlderFx>();
}

}  // namespace reno
