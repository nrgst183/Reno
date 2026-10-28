// Smart rendering's source analysis: see smartrender.h.
#include "smartrender.h"
#include "ffmpeg_common.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>

namespace reno {

namespace {

using namespace ff;

// What the analysis keeps about one packet.
struct PacketNote {
    int64_t pts = AV_NOPTS_VALUE, dts = AV_NOPTS_VALUE;
    int size = 0;
    bool key = false;           // a key packet that needs nothing before it
};

// The first video NAL unit's type of an H.264 or HEVC packet (-1: none found). 'lengthSize' is the size of the length
// prefix of AVCC/HVCC packets, 0 for start codes.
int firstVclNal(const AVPacket* pkt, bool hevc, int lengthSize)
{
    const uint8_t* d = pkt->data;
    const int n = pkt->size;
    int i = 0;
    while (i < n) {
        int nalStart, nalEnd;
        if (lengthSize > 0) {
            if (i + lengthSize > n) return -1;
            uint32_t len = 0;
            for (int k = 0; k < lengthSize; ++k) len = (len << 8) | d[i + k];
            nalStart = i + lengthSize;
            if (len == 0 || (int64_t)nalStart + len > n) return -1;
            nalEnd = nalStart + (int)len;
        } else {
            // a start code: 00 00 01 (or 00 00 00 01)
            while (i + 3 <= n && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) ++i;
            if (i + 3 >= n) return -1;
            nalStart = i + 3;
            nalEnd = nalStart + 1;
            while (nalEnd + 3 <= n && !(d[nalEnd] == 0 && d[nalEnd + 1] == 0 && (d[nalEnd + 2] == 1 || d[nalEnd + 2] == 0))) ++nalEnd;
            if (nalEnd + 3 > n) nalEnd = n;
        }
        if (nalStart >= n) return -1;
        const int type = hevc ? (d[nalStart] >> 1) & 0x3f : d[nalStart] & 0x1f;
        if (hevc ? type < 32 : (type >= 1 && type <= 5)) return type;
        i = nalEnd;
    }
    return -1;
}

// Whether a key packet starts a closed group of pictures: nothing after it needs anything before it.
bool closedKey(const AVCodecParameters* par, const AVPacket* pkt)
{
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    if (desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY)) return true;
    switch (par->codec_id) {
    case AV_CODEC_ID_H264: {
        const int ls = par->extradata_size > 4 && par->extradata[0] == 1 ? (par->extradata[4] & 3) + 1 : 0;
        return firstVclNal(pkt, false, ls) == 5;                    // an IDR picture
    }
    case AV_CODEC_ID_HEVC: {
        const int ls = par->extradata_size > 21 && par->extradata[0] == 1 ? (par->extradata[21] & 3) + 1 : 0;
        const int t = firstVclNal(pkt, true, ls);
        return t == 19 || t == 20;                                  // IDR_W_RADL, IDR_N_LP
    }
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_MPEG1VIDEO: {
        // the group of pictures header (00 00 01 B8) says whether the group is closed
        const uint8_t* d = pkt->data;
        for (int i = 0; i + 8 <= pkt->size; ++i)
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                if (d[i + 3] == 0xB8) return ((d[i + 7] >> 6) & 1) != 0;       // closed_gop
                if (d[i + 3] == 0x00) return false;                             // the picture came before any group header
            }
        return false;
    }
    default: return false;
    }
}

bool codecCopyable(const AVCodecParameters* par)
{
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    if (desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY)) return true;
    return par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC || par->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
           par->codec_id == AV_CODEC_ID_MPEG1VIDEO;
}

struct Cached {
    int64_t size = -1, mtime = -1;
    std::shared_ptr<const SmartSource> source;
    std::string why;
};
std::mutex gCacheMutex;
std::map<std::string, Cached> gCache;

bool statFile(const std::string& path, int64_t* size, int64_t* mtime)
{
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::u8path(path);
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) return false;
    const auto tm = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    *size = (int64_t)sz;
    *mtime = (int64_t)tm.time_since_epoch().count();
    return true;
}

bool analyze(const std::string& path, std::shared_ptr<const SmartSource>* out, std::string* why)
{
    AVFormatContext* raw = nullptr;
    int r = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (r < 0 || !raw) {
        *why = "cannot open " + pathFileName(path) + ": " + errText(r);
        return false;
    }
    InputPtr fc(raw);
    if (avformat_find_stream_info(fc.get(), nullptr) < 0) {
        *why = "cannot read " + pathFileName(path);
        return false;
    }
    const int vi = av_find_best_stream(fc.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        *why = "the file has no video";
        return false;
    }
    AVStream* st = fc->streams[vi];
    const AVCodecParameters* par = st->codecpar;
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        *why = "the file's picture is a cover";
        return false;
    }
    if (!codecCopyable(par)) {
        *why = std::string("the ") + avcodec_get_name(par->codec_id) + " video cannot be copied (smart rendering copies H.264, HEVC, MPEG-1, MPEG-2 and intra-only video)";
        return false;
    }
    AVRational fps = st->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) fps = st->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) {
        *why = "the frame rate is not known";
        return false;
    }
    if (st->start_time == AV_NOPTS_VALUE) {
        *why = "the stream has no start time";
        return false;
    }
    auto s = std::make_shared<SmartSource>();
    s->path = path;
    s->streamIndex = vi;
    s->codecId = (int)par->codec_id;
    s->codecName = avcodec_get_name(par->codec_id);
    s->width = par->width;
    s->height = par->height;
    s->pixFmt = par->format;
    s->profile = par->profile;
    s->level = par->level;
    s->fieldOrder = (int)par->field_order;
    s->colorRange = (int)par->color_range;
    s->colorSpace = (int)par->color_space;
    s->colorPrimaries = (int)par->color_primaries;
    s->colorTrc = (int)par->color_trc;
    AVRational sar = st->sample_aspect_ratio.num > 0 ? st->sample_aspect_ratio : par->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = av_make_q(1, 1);
    s->sarNum = sar.num;
    s->sarDen = sar.den;
    s->tbNum = st->time_base.num;
    s->tbDen = st->time_base.den;
    s->fpsNum = fps.num;
    s->fpsDen = fps.den;
    if (par->extradata && par->extradata_size > 0) s->extradata.assign(par->extradata, par->extradata + par->extradata_size);
    for (int k = 0; k < par->nb_coded_side_data; ++k)
        if (par->coded_side_data[k].type == AV_PKT_DATA_DISPLAYMATRIX) s->rotated = true;

    // every packet of the stream, in the order the file gives them (decoding order)
    std::vector<PacketNote> notes;
    PacketPtr pkt(av_packet_alloc());
    if (!pkt) {
        *why = "out of memory";
        return false;
    }
    while ((r = av_read_frame(fc.get(), pkt.get())) >= 0) {
        if (pkt->stream_index == vi) {
            if (pkt->flags & (AV_PKT_FLAG_CORRUPT | AV_PKT_FLAG_DISCARD)) {
                *why = "the video has damaged packets";
                return false;
            }
            PacketNote n;
            n.pts = pkt->pts;
            n.dts = pkt->dts;
            n.size = pkt->size;
            n.key = (pkt->flags & AV_PKT_FLAG_KEY) != 0 && closedKey(par, pkt.get());
            if (n.pts == AV_NOPTS_VALUE) {
                *why = "a packet has no time";
                return false;
            }
            notes.push_back(n);
        }
        av_packet_unref(pkt.get());
    }
    if (notes.empty()) {
        *why = "the video has no packets";
        return false;
    }
    // frame numbers: the stream's earliest time is frame 0; every packet must sit on the frame grid
    int64_t minPts = notes[0].pts;
    for (const PacketNote& n : notes) minPts = std::min(minPts, n.pts);
    if (minPts != st->start_time) {
        *why = "the first frame is not at the start of the stream";
        return false;
    }
    const AVRational frameDur = av_inv_q(fps);      // seconds per frame
    std::vector<int64_t> frameOf(notes.size());
    int64_t maxFrame = 0;
    for (size_t i = 0; i < notes.size(); ++i) {
        const AVRational t = av_make_q(1, 1);
        (void)t;
        // (pts - minPts) * time base / frame duration, to the nearest whole frame, which must be within a twentieth of it
        const int64_t num = (notes[i].pts - minPts) * st->time_base.num * frameDur.den;
        const int64_t den = (int64_t)st->time_base.den * frameDur.num;
        const int64_t f = av_rescale_rnd(num, 1, den, AV_ROUND_NEAR_INF);
        const int64_t back = av_rescale_rnd(f, den, 1, AV_ROUND_NEAR_INF);
        if (std::llabs(num - back) * 20 > den) {
            *why = "the frames are not on a constant frame rate";
            return false;
        }
        frameOf[i] = f;
        maxFrame = std::max(maxFrame, f);
    }
    s->frames = maxFrame + 1;

    // closed groups: from each closed key packet to the next
    struct Span { size_t first, last; };
    std::vector<size_t> starts;
    for (size_t i = 0; i < notes.size(); ++i)
        if (notes[i].key) starts.push_back(i);
    std::vector<Span> spans;
    for (size_t g = 0; g < starts.size(); ++g) spans.push_back({starts[g], (g + 1 < starts.size() ? starts[g + 1] : notes.size()) - 1});
    std::vector<SmartGop> gops;
    std::vector<std::pair<int64_t, int64_t>> ranges;        // [first, last] frame of each group, for the overlap check
    for (const Span& sp : spans) {
        int64_t lo = frameOf[sp.first], hi = lo;
        for (size_t i = sp.first; i <= sp.last; ++i) {
            lo = std::min(lo, frameOf[i]);
            hi = std::max(hi, frameOf[i]);
        }
        ranges.push_back({lo, hi});
    }
    for (size_t g = 0; g < spans.size(); ++g) {
        const Span& sp = spans[g];
        const int count = (int)(sp.last - sp.first + 1);
        const int64_t lo = ranges[g].first, hi = ranges[g].second;
        if (hi - lo + 1 != count) continue;                 // not a run of distinct consecutive frames
        std::vector<char> seen((size_t)count, 0);
        bool distinct = true;
        int delay = 0;
        int64_t minGroupPts = notes[sp.first].pts;
        for (size_t i = sp.first; i <= sp.last; ++i) {
            char& c = seen[(size_t)(frameOf[i] - lo)];
            if (c) distinct = false;
            c = 1;
            delay = std::max(delay, (int)((int64_t)(i - sp.first) - (frameOf[i] - lo)));
            minGroupPts = std::min(minGroupPts, notes[i].pts);
        }
        if (!distinct) continue;
        bool overlaps = false;                              // no other group may show any of these frames
        for (size_t o = 0; o < spans.size() && !overlaps; ++o)
            if (o != g && ranges[o].first <= hi && ranges[o].second >= lo) overlaps = true;
        if (overlaps) continue;
        SmartGop gop;
        gop.startFrame = lo;
        gop.frames = count;
        gop.keyDts = notes[sp.first].dts;
        gop.keyPts = notes[sp.first].pts;
        gop.keySize = notes[sp.first].size;
        gop.minPts = minGroupPts;
        gop.delay = std::max(0, delay);
        gops.push_back(gop);
    }
    std::sort(gops.begin(), gops.end(), [](const SmartGop& a, const SmartGop& b) { return a.startFrame < b.startFrame; });
    s->gops = gops;
    if (gops.empty()) {
        *why = "the video has no closed groups of pictures to copy";
        return false;
    }
    *out = s;
    return true;
}

}  // namespace

bool analyzeSmartSource(const std::string& path, std::shared_ptr<const SmartSource>* out, std::string* why)
{
    std::string dummy;
    if (!why) why = &dummy;
    int64_t size = -1, mtime = -1;
    statFile(path, &size, &mtime);
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        auto it = gCache.find(path);
        if (it != gCache.end() && it->second.size == size && it->second.mtime == mtime) {
            *out = it->second.source;
            *why = it->second.why;
            return it->second.source != nullptr;
        }
    }
    Cached c;
    c.size = size;
    c.mtime = mtime;
    std::shared_ptr<const SmartSource> made;
    std::string reason;
    if (analyze(path, &made, &reason)) c.source = made;
    c.why = reason;
    {
        std::lock_guard<std::mutex> lk(gCacheMutex);
        gCache[path] = c;
    }
    *out = c.source;
    *why = c.why;
    return c.source != nullptr;
}

void forgetSmartSources()
{
    std::lock_guard<std::mutex> lk(gCacheMutex);
    gCache.clear();
}

std::vector<std::string> smartHeaderSets(int codecId, const uint8_t* d, int n)
{
    std::vector<std::string> out;
    const bool h264 = codecId == AV_CODEC_ID_H264, hevc = codecId == AV_CODEC_ID_HEVC;
    if (n <= 0 || !d) return out;
    if (!h264 && !hevc) {
        out.emplace_back((const char*)d, (size_t)n);
        return out;
    }
    auto keep = [&](const uint8_t* nal, int len) {
        if (len < 2) return;
        const int type = h264 ? (nal[0] & 0x1f) : ((nal[0] >> 1) & 0x3f);
        const bool wanted = h264 ? (type == 7 || type == 8 || type == 13) : (type >= 32 && type <= 34);
        if (wanted) out.emplace_back((const char*)nal, (size_t)len);
    };
    if (d[0] == 1) {                                    // avcC / hvcC
        int at = 0;
        if (h264 && n > 6) {
            at = 5;
            for (int pass = 0; pass < 2 && at < n; ++pass) {
                int count = d[at++];
                if (pass == 0) count &= 0x1f;
                for (int k = 0; k < count && at + 2 <= n; ++k) {
                    const int len = (d[at] << 8) | d[at + 1];
                    at += 2;
                    if (at + len > n) return out;
                    keep(d + at, len);
                    at += len;
                }
            }
        } else if (hevc && n > 23) {
            at = 22;
            const int arrays = d[at++];
            for (int a = 0; a < arrays && at + 3 <= n; ++a) {
                at += 1;
                const int count = (d[at] << 8) | d[at + 1];
                at += 2;
                for (int k = 0; k < count && at + 2 <= n; ++k) {
                    const int len = (d[at] << 8) | d[at + 1];
                    at += 2;
                    if (at + len > n) return out;
                    keep(d + at, len);
                    at += len;
                }
            }
        }
        return out;
    }
    // Annex B: NAL units after 00 00 01 (or 00 00 00 01)
    int start = -1;
    for (int i = 0; i + 2 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start >= 0) {
                int end = i;
                while (end > start && d[end - 1] == 0) --end;
                keep(d + start, end - start);
            }
            start = i + 3;
            i += 2;
        }
    }
    if (start >= 0) keep(d + start, n - start);
    return out;
}

bool smartPacketToAnnexB(int codecId, const std::vector<uint8_t>& ex, const uint8_t* in, int size, std::vector<uint8_t>* out)
{
    out->clear();
    const bool h264 = codecId == AV_CODEC_ID_H264, hevc = codecId == AV_CODEC_ID_HEVC;
    if ((!h264 && !hevc) || ex.empty() || ex[0] != 1) return true;
    const int ls = h264 ? (ex.size() > 4 ? (ex[4] & 3) + 1 : 0) : (ex.size() > 21 ? (ex[21] & 3) + 1 : 0);
    if (ls < 1) return false;
    int at = 0;
    while (at < size) {
        if (at + ls > size) return false;
        uint32_t len = 0;
        for (int k = 0; k < ls; ++k) len = (len << 8) | in[at + k];
        at += ls;
        if ((int64_t)len > size - at) return false;
        out->push_back(0); out->push_back(0); out->push_back(0); out->push_back(1);
        out->insert(out->end(), in + at, in + at + len);
        at += (int)len;
    }
    return true;
}

}  // namespace reno
