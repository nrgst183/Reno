// Reading media files with FFmpeg: reno::openMedia() and reno::mediaBackendVersion().
#include "ffmpeg_common.h"
#include "hwdecode.h"
#include "../engine/profile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>
#include <filesystem>
#include <map>
#include <mutex>
#include <numeric>
#include <vector>
#include <atomic>
#include <memory>
#include <thread>

namespace reno {
namespace {

using namespace ff;

// ---------------------------------------------------------------- tuning
// Converted frames kept per open file, so that scrubbing around one spot and asking for the same
// frame again costs nothing.
const size_t kFrameCacheBytes = 128u * 1024u * 1024u;
// Decoded frames kept after a seek backwards, so stepping or playing backwards does not seek again for every frame.
const size_t kBackBufferBytes = 192u * 1024u * 1024u;
// ...and of those decoded going forwards (for a reversed event or a stutter going back over what was just played).
const size_t kForwardKeepBytes = 96u * 1024u * 1024u;
// A jump forwards of no more than this is always decoded through, never seeked.
const double kForwardDecodeSeconds = 0.25;
// A jump forwards of more than this seeks, when the file has no index to say where its key frames are.
const double kForwardSeekSeconds = 2.0;
// Decoding on to a frame further than this ahead, the frames before it that nothing depends on are not decoded
// (Demux::skipNonRefBefore) up to this much before it.
const double kSkipMarginSeconds = 0.5;
// A quick frame (MediaReadHints::quick) is the nearest one at hand when the one wanted is more than this ahead of it.
const double kQuickGapSeconds = 0.4;
// How far before the wanted sample an audio seek aims, so that the decoder has settled when it gets there.
const double kAudioPrerollSeconds = 0.25;
// Frames held after a seek and placed together, by where most of their time stamps put them (decodeAudio).
const int kAudioSyncFrames = 8;
// Frames decoded from the start whose sizes are learnt, to put the sound read after a seek on their step (placePending).
const int kAudioGridFrames = 17;
// A jump forwards in the sound of no more than this is decoded through rather than seeked.
const double kAudioSkipSeconds = 0.5;
// A hole in the audio time stamps longer than this (and shorter than kAudioMaxGapSeconds) is filled with silence.
const double kAudioGapSeconds = 0.25;
const double kAudioMaxGapSeconds = 10.0;
// Reading a file through for its length (scanEnd).
const int64_t kGuessedScanBytes = 64ll << 20;
const int64_t kMissingScanBytes = 512ll << 20;
// This many damaged packets in a row and decoding gives up.
const int kMaxDecodeErrors = 64;
// ...on the graphics card, where a failure usually means the card cannot do it: software takes over sooner.
const int kMaxHwDecodeErrors = 6;
// Positions further than this from zero (in samples) cannot be real.
const int64_t kMaxSamplePosition = (int64_t)1 << 46;

const int64_t kNoPts = AV_NOPTS_VALUE;
const int64_t kForever = std::numeric_limits<int64_t>::max();

// VEGAS's start of AAC sound in an MP4/MOV/M4A file.
int64_t aacDelaySamples(const std::string& format, AVCodecID codec, int profile)
{
    if (codec != AV_CODEC_ID_AAC) return 0;
    if (format.find("mp4") == std::string::npos && format.find("mov") == std::string::npos) return 0;
    if (profile != AV_PROFILE_UNKNOWN && profile != AV_PROFILE_AAC_LOW) return 0;
    return 2112;
}

void setError(std::string* error, const std::string& text)
{
    if (error) *error = text;
}

// ---------------------------------------------------------------- one demuxer + one decoder
struct Demux {
    std::unique_ptr<hw::State> hw;  // set when this decoder works on the graphics card
    std::shared_ptr<void> hwToken;  // its place among the decoders on the card
    InputPtr fmt;
    CodecPtr dec;
    PacketPtr pkt;
    FramePtr frame;                 // where frames are received before they are moved somewhere else
    int index = -1;                 // the stream being decoded
    AVStream* st = nullptr;
    bool draining = false;          // the end of the file was reached and the decoder told so
    bool ended = false;             // the decoder has given its last frame
    int errors = 0;                 // damaged packets in a row
    int64_t lastPacketTs = kNoPts;  // decoding time of the last packet given to the decoder
    int64_t skipUntil = kNoPts;     // packets that end before this are thrown away undecoded
    bool pristine = true;           // nothing has been read since the file was opened
    // video: frames that start before this and that no other frame is made from.
    int64_t skipNonRefBefore = kNoPts;
    std::shared_ptr<void> videoToken;     // counts this among the open video decoders
};

// How many threads a video decoder being opened gets.
std::atomic<int> gVideoDecoders{0};
std::atomic<int> gDecoderThreadLimit{0};
}  // namespace
void setVideoDecoderThreadLimit(int n) { gDecoderThreadLimit.store(std::max(0, std::min(64, n))); }
int videoDecoderThreadLimit() { return gDecoderThreadLimit.load(); }
namespace {
int videoDecoderThreads()
{
    if (gDecoderThreadLimit.load() > 0) return gDecoderThreadLimit.load();
    const int open = gVideoDecoders.load();
    if (open < 4) return processorLimit();                      // as many as there are processors
    const int hw = processorCount();
    if (open < 16) return std::min(hw, 4);
    return std::min(hw, 2);
}
std::shared_ptr<void> videoDecoderToken()
{
    gVideoDecoders.fetch_add(1);
    return std::shared_ptr<void>((void*)1, [](void*) { gVideoDecoders.fetch_sub(1); });
}

bool openInput(const std::string& path, InputPtr& out, std::string* error)
{
    AVFormatContext* fc = nullptr;
    int r;
    std::string first;
    double seqFps = 0;
    if (parseImageSequenceSpec(path, first, seqFps)) {
        // an image sequence (media.h): FFmpeg's frame-file reader, from the first frame's number, at the rate asked
        std::string pattern;
        int start = 0;
        if (!imageSequencePattern(first, pattern, start)) {
            setError(error, pathFileName(first) + " has no number in its name to make a sequence of.");
            return false;
        }
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "pattern_type", "sequence", 0);
        av_dict_set(&opts, "start_number", std::to_string(start).c_str(), 0);
        av_dict_set(&opts, "framerate", strfmt("%.6f", seqFps).c_str(), 0);
        r = avformat_open_input(&fc, pattern.c_str(), av_find_input_format("image2"), &opts);
        av_dict_free(&opts);
    } else {
        r = avformat_open_input(&fc, path.c_str(), nullptr, nullptr);
    }
    if (r < 0 || !fc) {
        setError(error, "Cannot open " + pathFileName(path) + ": " + errText(r) + ".");
        return false;
    }
    out.reset(fc);
    r = avformat_find_stream_info(fc, nullptr);
    if (r < 0) {
        setError(error, "Cannot read " + pathFileName(path) + ": " + errText(r) + ".");
        out.reset();
        return false;
    }
    return true;
}

// Opens the decoder for stream 'index' of d.fmt.
bool openStream(Demux& d, int index, AVMediaType type, std::string* error, bool tryHw = false)
{
    AVFormatContext* fc = d.fmt.get();
    if (!fc || index < 0 || index >= (int)fc->nb_streams || !fc->streams[index] || !fc->streams[index]->codecpar ||
        fc->streams[index]->codecpar->codec_type != type) {
        setError(error, "The file has changed since it was opened.");
        return false;
    }
    AVStream* st = fc->streams[index];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    // VP8 and VP9 with transparency (WebM's alpha_mode).
    if (type == AVMEDIA_TYPE_VIDEO && (st->codecpar->codec_id == AV_CODEC_ID_VP8 || st->codecpar->codec_id == AV_CODEC_ID_VP9)) {
        const AVDictionaryEntry* alpha = av_dict_get(st->metadata, "alpha_mode", nullptr, 0);
        if (alpha && alpha->value && std::strcmp(alpha->value, "1") == 0)
            if (const AVCodec* vpx = avcodec_find_decoder_by_name(st->codecpar->codec_id == AV_CODEC_ID_VP9 ? "libvpx-vp9" : "libvpx"))
                codec = vpx;
    }
    if (!codec) {
        setError(error, strfmt("No decoder for %s in this FFmpeg.", avcodec_get_name(st->codecpar->codec_id)));
        return false;
    }
    // On the graphics card first if it may be; if that decoder will not even start, the same stream in software.
    std::unique_ptr<hw::State> hwState;
    std::shared_ptr<void> hwToken;
    CodecPtr ctx;
    int r = 0;
    for (int attempt = tryHw && type == AVMEDIA_TYPE_VIDEO ? 0 : 1; attempt < 2; ++attempt) {
        ctx.reset(avcodec_alloc_context3(codec));
        if (!ctx) {
            setError(error, "Out of memory.");
            return false;
        }
        r = avcodec_parameters_to_context(ctx.get(), st->codecpar);
        if (r < 0) {
            setError(error, strfmt("Cannot set up the %s decoder: %s.", codec->name, errText(r).c_str()));
            return false;
        }
        ctx->pkt_timebase = st->time_base;
        hwState.reset();
        hwToken.reset();
        if (attempt == 0) {
            hwState.reset(new hw::State());
            if (!hw::attach(ctx.get(), codec, st->codecpar, *hwState, hwToken)) {
                hwState.reset();
                ctx.reset();
                continue;
            }
        }
        if (type == AVMEDIA_TYPE_VIDEO && !hwState) {
            ctx->thread_count = videoDecoderThreads();
            ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        if (hwState && hwState->emulated) {
            ctx->thread_count = 1;
            ctx->thread_type = FF_THREAD_SLICE;
        }
        r = avcodec_open2(ctx.get(), codec, nullptr);
        if (r >= 0) break;
        if (attempt == 1) {
            setError(error, strfmt("Cannot start the %s decoder: %s.", codec->name, errText(r).c_str()));
            return false;
        }
        ctx.reset();
        hwState.reset();
        hwToken.reset();
    }
    if (!ctx) {
        setError(error, "Out of memory.");
        return false;
    }
    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!pkt || !frame) {
        setError(error, "Out of memory.");
        return false;
    }
    d.hw = std::move(hwState);
    d.hwToken = std::move(hwToken);
    d.dec = std::move(ctx);
    d.videoToken = type == AVMEDIA_TYPE_VIDEO && !d.hw ? videoDecoderToken() : nullptr;
    d.pkt = std::move(pkt);
    d.frame = std::move(frame);
    d.index = index;
    d.st = st;
    d.draining = d.ended = false;
    d.errors = 0;
    d.lastPacketTs = d.skipUntil = kNoPts;
    d.pristine = true;
    return true;
}

// The next decoded frame of d's stream. 1 = 'out' holds a frame, 0 = the stream has ended,
// negative = an FFmpeg error after too many damaged packets in a row.
int receiveFrame(Demux& d, AVFrame* out)
{
    if (d.ended) return 0;
    AVCodecContext* dec = d.dec.get();
    AVPacket* pkt = d.pkt.get();
    for (;;) {
        int r = avcodec_receive_frame(dec, out);
        if (r >= 0) {
            d.errors = 0;
            return 1;
        }
        if (r == AVERROR_EOF) {
            d.ended = true;
            return 0;
        }
        if (r != AVERROR(EAGAIN)) {
            // A damaged frame. Carry on with the next one unless nothing has decoded for a long stretch.
            if (++d.errors > (d.hw ? kMaxHwDecodeErrors : kMaxDecodeErrors)) return r;
            continue;
        }
        if (d.draining) {           // the decoder was told about the end but wants more
            d.ended = true;
            return 0;
        }
        // The decoder wants more data: find the next packet of our stream.
        d.pristine = false;
        int tries = 0;
        for (;;) {
            r = av_read_frame(d.fmt.get(), pkt);        // on failure the packet is left blank
            if (r == AVERROR(EAGAIN) && ++tries < 1000) continue;
            if (r < 0) break;
            if (pkt->stream_index != d.index) {
                av_packet_unref(pkt);
                continue;
            }
            if (d.skipUntil != kNoPts) {
                const int64_t ts = pkt->pts != kNoPts ? pkt->pts : pkt->dts;
                if (ts != kNoPts && ts + std::max<int64_t>(pkt->duration, 0) <= d.skipUntil) {
                    av_packet_unref(pkt);
                    continue;
                }
                d.skipUntil = kNoPts;
            }
            break;
        }
        if (r < 0) {
            // The end of the file (a read error is treated the same way): ask the decoder for what it is still holding.
            avcodec_send_packet(dec, nullptr);
            d.draining = true;
            continue;
        }
        d.lastPacketTs = pkt->dts != kNoPts ? pkt->dts : pkt->pts;
        if (dec->codec_type == AVMEDIA_TYPE_VIDEO) {
            const bool skip = d.skipNonRefBefore != kNoPts && pkt->pts != kNoPts && pkt->pts < d.skipNonRefBefore && !(pkt->flags & AV_PKT_FLAG_KEY);
            const AVDiscard want = skip ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
            if (dec->skip_frame != want) dec->skip_frame = want;
        }
        r = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            if (++d.errors > (d.hw ? kMaxHwDecodeErrors : kMaxDecodeErrors)) return r;
        }
    }
}

// How many packets stream 'index' of the file has, counting no further than 'limit'.
int countPackets(const std::string& path, int index, int limit)
{
    InputPtr fmt;
    if (!openInput(path, fmt, nullptr)) return 0;
    PacketPtr pkt(av_packet_alloc());
    if (!pkt) return 0;
    int count = 0;
    for (int read = 0; count < limit && read < 100000; ++read) {
        if (av_read_frame(fmt.get(), pkt.get()) < 0) break;
        if (pkt->stream_index == index) ++count;
        av_packet_unref(pkt.get());
    }
    return count;
}

// Where the last packet of the streams 'vi' and 'ai' ends, in microseconds of FFmpeg's time stamps, read from the
// packets themselves (kNoPts if none says).
int64_t scanEnd(const std::string& path, int vi, int ai, int64_t frameTicks, int64_t most)
{
    static std::mutex mutex;
    static std::map<std::string, int64_t> known;
    int64_t size = 0, stamp = 0;
    {
        std::error_code ec;
        const std::filesystem::path fp = std::filesystem::u8path(path);
        const auto sz = std::filesystem::file_size(fp, ec);
        if (!ec) size = (int64_t)sz;
        const auto t = std::filesystem::last_write_time(fp, ec);
        if (!ec) stamp = (int64_t)t.time_since_epoch().count();
    }
    const std::string key = strfmt("%lld|%lld|%d|%d|", (long long)size, (long long)stamp, vi, ai) + path;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = known.find(key);
        if (it != known.end()) return it->second;
    }
    int64_t endUs = kNoPts;
    InputPtr fmt;
    if (openInput(path, fmt, nullptr)) {
        AVFormatContext* fc = fmt.get();
        for (unsigned i = 0; i < fc->nb_streams; ++i)
            if (fc->streams[i]) fc->streams[i]->discard = ((int)i == vi || (int)i == ai) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        if (size > most && fc->pb) av_seek_frame(fc, -1, size - most, AVSEEK_FLAG_BYTE);
        PacketPtr pkt(av_packet_alloc());
        int64_t last[2] = {kNoPts, kNoPts}, first[2] = {kNoPts, kNoPts}, total[2] = {0, 0};
        const int idx[2] = {vi, ai};
        for (int64_t n = 0; pkt && n < 50000000; ++n) {
            if (av_read_frame(fc, pkt.get()) < 0) break;
            for (int k = 0; k < 2; ++k) {
                if (pkt->stream_index != idx[k] || idx[k] < 0) continue;
                const AVStream* st = fc->streams[idx[k]];
                int64_t ts = pkt->pts != kNoPts ? pkt->pts : pkt->dts;
                int64_t dur = pkt->duration > 0 ? pkt->duration : (k == 0 ? frameTicks : 0);
                if (ts == kNoPts) ts = last[k] != kNoPts ? last[k] : (st->start_time != kNoPts ? st->start_time : 0);
                int64_t end = ts + std::max<int64_t>(dur, 0);
                if (last[k] == kNoPts || end > last[k]) last[k] = end;
                if (first[k] == kNoPts) first[k] = ts;
                total[k] += std::max<int64_t>(dur, 0);
                if (size <= most) end = std::max(end, first[k] + total[k]);
                const int64_t us = av_rescale_q(end, st->time_base, microseconds());
                if (endUs == kNoPts || us > endUs) endUs = us;
            }
            av_packet_unref(pkt.get());
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (known.size() > 4096) known.clear();
    known[key] = endUs;
    return endUs;
}

bool endsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Clockwise quarter turns (0, 90, 180, 270) needed to show the stream's frames upright.
int streamRotation(const AVStream* st)
{
    const int32_t* matrix = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 31, 102)
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (sd && sd->data && sd->size >= 9 * sizeof(int32_t)) matrix = (const int32_t*)sd->data;
#else
    size_t size = 0;
    const uint8_t* data = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, &size);
    if (data && size >= 9 * sizeof(int32_t)) matrix = (const int32_t*)data;
#endif
    if (!matrix) return 0;
    const double ccw = av_display_rotation_get(matrix);     // how far the matrix turns the frame anticlockwise
    if (!(ccw == ccw)) return 0;                            // NaN: not a usable matrix
    int quarter = (int)std::lround(-ccw / 90.0) % 4;
    if (quarter < 0) quarter += 4;
    return quarter * 90;
}

inline void copyOpaque(uint8_t* d, const uint8_t* s)
{
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
    d[3] = 255;
}

inline uint8_t mul255(unsigned c, unsigned a)   // c * a / 255, rounded
{
    const unsigned t = c * a + 128;
    return (uint8_t)((t + (t >> 8)) >> 8);
}

inline void copyPremultiplied(uint8_t* d, const uint8_t* s)
{
    const unsigned a = s[3];
    d[0] = mul255(s[0], a);
    d[1] = mul255(s[1], a);
    d[2] = mul255(s[2], a);
    d[3] = (uint8_t)a;
}

// ---------------------------------------------------------------- the reader
class FFReader final : public MediaReader {
public:
    FFReader() {}
    bool open(const std::string& path, std::string* error);

    const MediaInfo& info() const override { return info_; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override { double a, b; return videoFrameSpan(seconds, maxW, maxH, &a, &b); }
    ImagePtr videoFrameSpan(double seconds, int maxW, int maxH, double* from, double* until) override;
    ImagePtr videoFrameInDepth(double seconds, int maxW, int maxH, PixelDepth depth) override;
    int harvestKept(int maxW, int maxH, int most, const std::function<void(double, double, const ImagePtr&)>& put) override;
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override;
    bool audioPick(int64_t start, int frames, int sampleRate, int first, int count, float* left, float* right) override;
    bool audioChannels(int64_t start, int frames, int sampleRate, int channels, float* const* out) override;
    void setDeinterlace(int mode) override;
    void setFieldOrder(const std::string& order) override { fieldOverride_ = order; }
    int videoCost(double seconds, int maxW, int maxH) override;
    std::string hardwareDecoder() const override { return v_.hw ? v_.hw->name : std::string(); }

private:
    // A decoded frame and the time it starts showing.
    struct Shown {
        FramePtr f;
        int64_t pts = 0;
        bool valid = false;
    };
    struct CacheKey {
        int w, h;
        int64_t pts;
        bool operator<(const CacheKey& o) const
        {
            if (w != o.w) return w < o.w;
            if (h != o.h) return h < o.h;
            return pts < o.pts;
        }
    };
    struct CacheEntry {
        CacheKey key;
        int64_t until;              // the frame shows for key.pts <= time < until
        ImagePtr img;
    };
    enum Advance { kFound, kTooLate, kFailed, kCancelled };
    // A frame kept for going backwards, and when the next one starts (kNoPts: not known).
    struct BackFrame {
        FramePtr f;
        int64_t until = kNoPts;
        size_t bytes = 0;
        uint64_t run = 0;           // the stretch decoded
        bool shared = false;        // harvestKept gave it away
    };

    // frames
    void outputSize(int maxW, int maxH, int& ow, int& oh) const;
    int64_t targetTick(double seconds) const;
    Shown* locate(int64_t target);
    Advance advance(int64_t target, Shown** out);
    int decodeVideoFrame();
    Shown* locateReady(int64_t target);         // locate(), the frame in ordinary memory
    bool swapToSoftware(bool reposition);       // the graphics card failed
    bool worthSeekingForward(int64_t target);
    bool seekVideo(int64_t target, int attempt);
    void resetVideoState(bool atStart, int64_t seekTs);
    ImagePtr convert(const AVFrame* f, int ow, int oh);
    ImagePtr convertFloat(const AVFrame* f, int ow, int oh);
    ImagePtr cacheFind(int w, int h, int64_t target, int64_t* pts = nullptr, int64_t* until = nullptr);
    // The times (seconds) whose target tick lies in [lo, hi) of ticks: what a frame shown for those ticks is good for.
    void tickSpanSeconds(int64_t lo, int64_t hi, double* from, double* until) const;
    static void stillSpan(double* from, double* until) { *from = -std::numeric_limits<double>::infinity(); *until = std::numeric_limits<double>::infinity(); }
    void cacheClear();
    void keepForBackwards(const AVFrame* f, int64_t pts);
    Shown* fromBackBuffer(int64_t target);
    void deinterlaceRows(uint8_t* rows, int stride, int w, int h, bool topFirst);
    void cacheInsert(int w, int h, int64_t pts, int64_t until, const ImagePtr& img);

    // sound
    bool ensureAudioOpen();
    void setupAudioStreams();
    bool seekAudio(int64_t start);
    void resetAudioState();
    // aDelay_ at sample rate 'rate' (the stream's own rate is info_.sampleRate), and in microseconds
    int64_t delayAt(int rate) const { return aDelay_ && info_.sampleRate > 0 ? av_rescale(aDelay_, rate, info_.sampleRate) : 0; }
    int64_t delayUs() const { return delayAt(1000000); }
    bool decodeAudio();
    bool framePosition(const AVFrame* f, int rate, int64_t* pos) const;
    bool placeFrame(AVFrame* f, int inRate, bool holes);
    bool placePending();
    void learnAudioGrid();
    bool configureResampler(const AVFrame* f, int inRate);
    bool resample(const uint8_t** in, int count);
    void flushResampler();
    void fifoDrop(int64_t n);
    int64_t fifoAvailable() const { return (int64_t)fifo_[0].size() - (int64_t)fifoRead_; }

    // both
    bool rewind(Demux& d, AVMediaType type, int refIndex);
    static void keepOnly(AVFormatContext* fc, int a, int b);

    MediaInfo info_;
    int64_t originUs_ = 0;          // the file's time zero, in microseconds of FFmpeg's time stamps
    int vIndex_ = -1, aIndex_ = -1; // the streams used, -1 if none
    int realVideoIndex_ = -1;       // the file's video stream even if we do not show it

    // ---- frames
    Demux v_;
    AVRational vTb_ = {1, 1};
    int64_t vStartPts_ = 0;         // time zero in the video stream's units
    int64_t vDefaultDuration_ = 1;  // one frame, in the stream's units
    int rotation_ = 0;              // clockwise degrees to turn decoded frames by
    Shown prev_, cur_;              // the last two frames decoded, in display order
    bool vEnded_ = false;           // cur_ is the last frame of the file
    bool vFresh_ = true;            // the demuxer is at the very start
    bool firstKnown_ = false, lastKnown_ = false;
    int64_t firstPts_ = 0, lastPts_ = 0;
    int64_t vExpectedPts_ = kNoPts; // where the next frame should start if it carries no time stamp
    int64_t vSeekTs_ = 0;           // where the last seek aimed
    bool hwTry_ = true;             // the graphics card may decode this file
    int64_t decodeTarget_ = kNoPts;     // the frame locate() is decoding on to
    int64_t decodeKeepTicks_ = 0;       // frames this much before it are kept going forwards, on the card
    bool keepThis_ = false;         // decodeVideoFrame
    int64_t resumeAfter_ = kNoPts;  // after the card failed
    SwsPtr sws_;
    int swsSrcW_ = 0, swsSrcH_ = 0, swsSrcFmt_ = -1, swsDstW_ = 0, swsDstH_ = 0, swsSpace_ = -1, swsFull_ = -1, swsFlags_ = 0;
    MemPtr tmp_;                    // swscale's output, top row first, before it is flipped into an Image
    size_t tmpBytes_ = 0;
    std::list<CacheEntry> lru_;     // most recently used first
    std::map<CacheKey, std::list<CacheEntry>::iterator> cacheIndex_;
    size_t cacheBytes_ = 0;
    std::map<int64_t, BackFrame> back_;     // frames kept for going backwards, by when they start
    size_t backBytes_ = 0;
    bool collecting_ = false;       // decoding after a seek backwards
    int64_t lastBackPts_ = kNoPts;  // the last frame kept
    uint64_t backRun_ = 1;          // counts the stretches decoded (BackFrame::run)
    Shown backShown_;               // a frame handed out from back_
    int64_t backUntil_ = kNoPts;
    int deint_ = 0;                 // setDeinterlace()
    std::string fieldOverride_;     // setFieldOrder()
    SwsPtr sws2_;                   // deinterlacing
    int sws2SrcW_ = 0, sws2SrcH_ = 0, sws2DstW_ = 0, sws2DstH_ = 0;
    MemPtr tmp2_;
    size_t tmp2Bytes_ = 0;
    std::vector<uint8_t> lineCopy_;
    SwsPtr swsDeep_;                // deep frames to 16-bit RGBA (videoFrameInDepth)
    int deepSrcW_ = 0, deepSrcH_ = 0, deepSrcFmt_ = -1, deepDstW_ = 0, deepDstH_ = 0, deepSpace_ = -1, deepFull_ = -1, deepFlags_ = 0;
    std::vector<uint16_t> deep_;
    // HDR and linear frames made SDR (toneMapped): the result (16-bit RGBA, full size), the converter to it and tables
    const AVFrame* toneMapped(const AVFrame* f);
    FramePtr tm_;
    SwsPtr tmSws_;
    int tmSrcW_ = 0, tmSrcH_ = 0, tmSrcFmt_ = -1, tmSpace_ = -1, tmFull_ = -1, tmLight_ = -1;
    std::vector<float> tmIn_;
    std::vector<uint16_t> tmOut_;

    // ---- sound
    Demux a_;
    bool aOpenTried_ = false;
    int aRef_ = -1;                 // the stream seeks are made on
    bool aSkipSeek_ = false;        // the format cannot seek accurately
    // The samples (at the stream's own rate) at the start of the decoded sound that VEGAS leaves out: AAC's encoder
    // delay in an MP4/MOV (aacDelaySamples).
    int64_t aDelay_ = 0;
    int outRate_ = 0;               // the sample rate of the last request
    SwrPtr swr_;
    Layout swrLayout_;              // what the resampler was built for
    int swrFmt_ = -1, swrRate_ = 0;
    int swrOutRate_ = 0, swrPickFirst_ = -1, swrPickCount_ = 0;
    bool swrMono_ = false;
    // audioPick(): which of the file's own channels the resampler gives (pickFirst_ < 0: all, mixed to stereo).
    int pickFirst_ = -1, pickCount_ = 0;
    // Resamplers let go of at a seek, kept for the next ones.
    struct ParkedSwr { SwrPtr swr; int fmt = -1, inRate = 0, outRate = 0, pickFirst = -1, pickCount = 0; bool mono = false; uint64_t mask = 0; int channels = 0; };
    std::vector<ParkedSwr> parked_;
    void parkResampler();
    void setPick(int first, int count);
    bool readAudio(int64_t start, int frames, int sampleRate, float* left, float* right);
    std::vector<float> fifo_[2];    // resampled sound not handed out yet
    size_t fifoRead_ = 0;
    int64_t fifoPos_ = 0;           // the position
    std::vector<float> conv_[2];
    bool aPipeline_ = false;        // the decoder is positioned and fifoPos_ means something
    bool aSynced_ = false;          // a frame has been decoded since the last seek
    bool aEnded_ = false;
    bool aFromStart_ = false;       // the last seek went to the very start of the stream
    bool aHeadIsStart_ = false;     // the next sample to hand out is the stream's first
    int64_t aSeekUs_ = 0;           // where the last seek aimed
    int64_t aInPos_ = 0;            // position (in samples at aInRate_) expected for the next decoded frame
    int aInRate_ = 0;
    bool aEndKnown_ = false;
    int64_t aEndPos_ = 0;           // first position past the end of the sound, at outRate_
    std::vector<FramePtr> pending_; // frames decoded since a seek, not yet placed
    // Matroska and WebM: FFmpeg takes the codec's delay.
    int64_t aStampFixNs_ = 0;
    // Step the stream's audio frames come in (gcd of their sizes) and where it starts, at aGridRate_.
    int64_t aGrid_ = 0, aGridOrigin_ = 0;
    int aGridRate_ = 0, aGridFrames_ = 0;
    bool aGridTried_ = false;

    // ---- sound in 5.1 (audioChannels with six): a decoder of its own, read from the start of the file and on in
    struct Multi {
        Demux d;
        SwrPtr swr;
        Layout in;
        int fmt = -1, inRate = 0, outRate = 0;
        std::vector<float> fifo[6];
        size_t read = 0;
        int64_t pos = 0;            // the position
        bool ended = false, failed = false;
    };
    std::unique_ptr<Multi> multi_;
    bool multiDecode(Multi& m);
};

void FFReader::keepOnly(AVFormatContext* fc, int a, int b)
{
    for (unsigned i = 0; i < fc->nb_streams; ++i)
        if (fc->streams[i]) fc->streams[i]->discard = ((int)i == a || (int)i == b) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

bool FFReader::open(const std::string& path, std::string* error)
{
    info_ = MediaInfo();
    info_.path = path;

    InputPtr fmt;
    if (!openInput(path, fmt, error)) return false;
    AVFormatContext* fc = fmt.get();
    const std::string format = fc->iformat && fc->iformat->name ? fc->iformat->name : "";

    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vi >= 0 ? vi : -1, nullptr, 0);
    std::string why;

    // ---- is the sound usable?
    if (ai >= 0) {
        const AVCodecParameters* par = fc->streams[ai]->codecpar;
        if (par->sample_rate < 1 || par->ch_layout.nb_channels < 1) {
            ai = -1;
        } else if (!avcodec_find_decoder(par->codec_id)) {
            why = strfmt("No decoder for %s in this FFmpeg.", avcodec_get_name(par->codec_id));
            ai = -1;
        }
    }
    // ---- is the frame usable?
    if (vi >= 0) {
        const AVStream* st = fc->streams[vi];
        const AVCodecParameters* par = st->codecpar;
        const bool cover = (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        if (!cover) realVideoIndex_ = vi;
        if (par->width < 1 || par->height < 1 || st->time_base.num < 1 || st->time_base.den < 1) {
            vi = -1;
        } else if (cover && ai >= 0) {
            vi = -1;                // a sound file with its cover frame attached is a sound file
        } else if (!avcodec_find_decoder(par->codec_id)) {
            why = strfmt("No decoder for %s in this FFmpeg.", avcodec_get_name(par->codec_id));
            vi = -1;
        }
    }
    if (vi >= 0) {
        v_.fmt = std::move(fmt);
        std::string e;
        if (!openStream(v_, vi, AVMEDIA_TYPE_VIDEO, &e, hwTry_)) {
            why = e;
            fmt = std::move(v_.fmt);
            vi = -1;
        }
    }
    if (vi < 0 && ai < 0) {
        setError(error, why.empty() ? pathFileName(path) + " has no pictures or sound that can be read." : pathFileName(path) + " cannot be read. " + why);
        return false;
    }
    vIndex_ = vi;
    aIndex_ = ai;
    info_.hasVideo = vi >= 0;
    info_.hasAudio = ai >= 0;

    const AVRational us = microseconds();
    bool reopenVideo = false;
    // ---- frames
    if (vi >= 0) {
        AVStream* st = fc->streams[vi];
        const AVCodecParameters* par = st->codecpar;
        vTb_ = st->time_base;
        if (st->start_time != kNoPts) vStartPts_ = st->start_time;
        else if (fc->start_time != kNoPts) vStartPts_ = av_rescale_q(fc->start_time, us, vTb_);
        else vStartPts_ = 0;
        originUs_ = av_rescale_q(vStartPts_, vTb_, us);

        // the shape it is shown at: non-square pixels widen or heighten it, a rotation may turn it on its side
        int w = par->width, h = par->height;
        const AVRational sar = av_guess_sample_aspect_ratio(fc, st, nullptr);
        if (sar.num > 0 && sar.den > 0) {
            const double a = av_q2d(sar);
            if (a > 1.0005 && a < 100.0) w = (int)std::lround(w * a);
            else if (a < 0.9995 && a > 0.01) h = (int)std::lround(h / a);
        }
        rotation_ = streamRotation(st);
        info_.rotation = rotation_;
        info_.pixelAspect = sar.num > 0 && sar.den > 0 && av_q2d(sar) > 0.01 && av_q2d(sar) < 100.0 ? av_q2d(sar) : 1.0;
        if (rotation_ == 90 || rotation_ == 270) std::swap(w, h);
        info_.width = std::max(1, std::min(w, 32768));
        info_.height = std::max(1, std::min(h, 32768));
        info_.videoCodec = avcodec_get_name(par->codec_id);
        if (const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get((AVPixelFormat)par->format)) {
            if (pd->nb_components > 0) info_.bitDepth = std::max(1, pd->comp[0].depth);
            info_.hasAlpha = (pd->flags & AV_PIX_FMT_FLAG_ALPHA) != 0 || (pd->flags & AV_PIX_FMT_FLAG_PAL) != 0;
        }
        if (v_.dec && (v_.dec->pix_fmt == AV_PIX_FMT_YUVA420P || std::strncmp(v_.dec->codec->name, "libvpx", 6) == 0)) info_.hasAlpha = true;
        if (par->field_order == AV_FIELD_TT || par->field_order == AV_FIELD_TB) info_.fieldOrder = "upper";
        else if (par->field_order == AV_FIELD_BB || par->field_order == AV_FIELD_BT) info_.fieldOrder = "lower";

        AVRational rate = av_guess_frame_rate(fc, st, nullptr);
        if (rate.num < 1 || rate.den < 1) rate = st->avg_frame_rate;
        double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
        if (!(fps > 0.0) || fps > 1000.0) fps = 0.0;

        // a frame file?
        const bool imageFormat = format == "image2" || endsWith(format, "_pipe");
        if (ai < 0) {
            if (endsWith(format, "_pipe")) {
                info_.isStill = true;
            } else {
                const double seconds = st->duration != kNoPts ? st->duration * av_q2d(vTb_) : -1.0;
                const bool maybe = imageFormat || format == "gif" || format == "apng" || st->nb_frames == 1 ||
                                   (st->nb_frames == 0 && (seconds <= 0.0 || (fps > 0.0 && seconds < 1.5 / fps)));
                if (maybe && countPackets(path, vi, 2) < 2) info_.isStill = true;
            }
        }
        info_.fps = info_.isStill ? 0.0 : fps;
        const double frameTicks = fps > 0.0 ? 1.0 / (fps * av_q2d(vTb_)) : 0.0;
        vDefaultDuration_ = frameTicks >= 1.0 && frameTicks < 1e15 ? (int64_t)std::llround(frameTicks) : 1;

        keepOnly(fc, vi, -1);
        if (st->start_time == kNoPts && fc->start_time == kNoPts) {
            // A file that does not say when its frames start (a bare MPEG-2 stream): its first frame says.
            FramePtr probe(av_frame_alloc());
            if (probe && receiveFrame(v_, probe.get()) > 0) {
                const int64_t ts = probe->best_effort_timestamp != kNoPts ? probe->best_effort_timestamp : probe->pts;
                if (ts != kNoPts) {
                    vStartPts_ = ts;
                    originUs_ = av_rescale_q(vStartPts_, vTb_, us);
                }
            }
            reopenVideo = true;
        }
        // DV is always lower field first (when standard definition), whether or not the file says so
        if (par->codec_id == AV_CODEC_ID_DVVIDEO && info_.fieldOrder == "none" && (par->height == 480 || par->height == 576)) info_.fieldOrder = "lower";
        prev_.f.reset(av_frame_alloc());
        cur_.f.reset(av_frame_alloc());
        backShown_.f.reset(av_frame_alloc());
        if (!prev_.f || !cur_.f || !backShown_.f) {
            setError(error, "Out of memory.");
            return false;
        }
    }
    // ---- sound
    if (ai >= 0) {
        const AVStream* st = fc->streams[ai];
        const AVCodecParameters* par = st->codecpar;
        info_.sampleRate = par->sample_rate;
        info_.channels = par->ch_layout.nb_channels;
        // the channels' names (VEGAS's speaker names) and the layout's
        {
            Layout lay;
            if (par->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || av_channel_layout_check(&par->ch_layout) == 0 ||
                av_channel_layout_copy(&lay.l, &par->ch_layout) < 0)
                av_channel_layout_default(&lay.l, info_.channels);
            char desc[128] = {0};
            if (av_channel_layout_describe(&lay.l, desc, sizeof(desc)) > 0) info_.channelLayout = desc;
            bool sides = false;
            for (int c = 0; c < info_.channels && c < 64; ++c) {
                const AVChannel ch = av_channel_layout_channel_from_index(&lay.l, (unsigned)c);
                if (ch == AV_CHAN_SIDE_LEFT || ch == AV_CHAN_SIDE_RIGHT) sides = true;
            }
            for (int c = 0; c < info_.channels && c < 64; ++c) {
                const AVChannel ch = av_channel_layout_channel_from_index(&lay.l, (unsigned)c);
                std::string n;
                switch (ch) {
                case AV_CHAN_FRONT_LEFT: n = "L"; break;
                case AV_CHAN_FRONT_RIGHT: n = "R"; break;
                case AV_CHAN_FRONT_CENTER: n = info_.channels == 1 ? "" : "C"; break;
                case AV_CHAN_LOW_FREQUENCY: n = "LFE"; break;
                case AV_CHAN_SIDE_LEFT: n = "Ls"; break;
                case AV_CHAN_SIDE_RIGHT: n = "Rs"; break;
                case AV_CHAN_BACK_LEFT: n = sides ? "Lb" : "Ls"; break;
                case AV_CHAN_BACK_RIGHT: n = sides ? "Rb" : "Rs"; break;
                case AV_CHAN_BACK_CENTER: n = "Cs"; break;
                default: break;
                }
                info_.channelNames.push_back(n);
            }
        }
        info_.audioCodec = avcodec_get_name(par->codec_id);
        aDelay_ = aacDelaySamples(format, par->codec_id, par->profile);
        info_.audioDelaySamples = (int)aDelay_;
        if (vi < 0) {
            if (st->start_time != kNoPts) originUs_ = av_rescale_q(st->start_time, st->time_base, us);
            else if (fc->start_time != kNoPts) originUs_ = fc->start_time;
            else originUs_ = 0;
        }
        aSkipSeek_ = realVideoIndex_ < 0 && (format == "mp3" || format == "aac" || format == "ac3" || format == "eac3" ||
                                             format == "dts" || format == "truehd" || format == "loas");
        // Ogg Vorbis and FLAC: after a seek FFmpeg works the first page's time stamps out from the page's end less
        // its packets' lengths.
        if (format == "ogg" && par->codec_id != AV_CODEC_ID_OPUS) aSkipSeek_ = true;
        if (format.find("matroska") != std::string::npos && par->initial_padding > 0 && st->time_base.num > 0) {
            // Matroska and WebM keep time in whole milliseconds.
            const int padRate = par->codec_id == AV_CODEC_ID_OPUS ? 48000 : par->sample_rate;
            const int64_t delayNs = av_rescale(par->initial_padding, 1000000000, padRate);
            const int64_t unitNs = av_rescale_q(1, st->time_base, av_make_q(1, 1000000000));
            if (unitNs > 0) {
                const int64_t frac = (unitNs - delayNs % unitNs) % unitNs;          // where in a unit the frames start
                aStampFixNs_ = frac * 2 >= unitNs ? frac - unitNs : frac;
            }
            // Sound alone: its time zero is where the pre-skip ends, not the (rounded) start of the first frame.
            if (vi < 0 && st->start_time != kNoPts) {
                const int64_t delayTb = av_rescale_q(delayNs, av_make_q(1, 1000000000), st->time_base);
                originUs_ = av_rescale_q(st->start_time + delayTb, st->time_base, us);
            }
        }
    }
    // ---- how long it lasts
    if (!info_.isStill) {
        int64_t endUs = kNoPts;
        const int used[2] = {vi, ai};
        for (int k = 0; k < 2; ++k) {
            if (used[k] < 0) continue;
            const AVStream* st = fc->streams[used[k]];
            if (st->duration == kNoPts || st->duration <= 0) continue;
            const int64_t e = av_rescale_q((st->start_time != kNoPts ? st->start_time : 0) + st->duration, st->time_base, us);
            if (endUs == kNoPts || e > endUs) endUs = e;
            if (k == 0 && e > originUs_) info_.videoDuration = (double)(e - originUs_) / 1e6;
        }
        if (endUs == kNoPts && fc->duration != kNoPts && fc->duration > 0)
            endUs = (fc->start_time != kNoPts ? fc->start_time : 0) + fc->duration;
        if (endUs != kNoPts && endUs > originUs_) info_.duration = (double)(endUs - originUs_) / 1e6;
        const bool guessed = fc->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE;
        if (!(info_.duration > 0) || guessed) {
            const int64_t scannedUs = scanEnd(path, vi, ai, vi >= 0 ? vDefaultDuration_ : 0, guessed ? kGuessedScanBytes : kMissingScanBytes);
            if (scannedUs != kNoPts && scannedUs > originUs_) info_.duration = (double)(scannedUs - originUs_) / 1e6;
        }
    }
    // ---- the markers kept in the file (its chapters), for View > Event Media Markers
    for (unsigned c = 0; c < fc->nb_chapters && c < 1000; ++c) {
        const AVChapter* ch = fc->chapters[c];
        if (!ch) continue;
        const double at = (double)(av_rescale_q(ch->start, ch->time_base, us) - originUs_) / 1e6;
        const AVDictionaryEntry* title = av_dict_get(ch->metadata, "title", nullptr, 0);
        info_.markers.emplace_back(std::max(0.0, at), title && title->value ? std::string(title->value) : std::string());
    }

    if (reopenVideo) {
        Demux fresh;
        v_.dec.reset();
        v_.hw.reset();
        v_.hwToken.reset();
        if (!openInput(path, fresh.fmt, error) || !openStream(fresh, vi, AVMEDIA_TYPE_VIDEO, error, hwTry_)) return false;
        v_ = std::move(fresh);
        keepOnly(v_.fmt.get(), vi, -1);
    }
    // The file opened above serves the frames if there are any; the sound gets its own when it is first asked for.
    if (vi < 0) {
        a_.fmt = std::move(fmt);
        if (!openStream(a_, ai, AVMEDIA_TYPE_AUDIO, error)) return false;
        setupAudioStreams();
        aOpenTried_ = true;
    }
    return true;
}

// Puts d's demuxer back at the very start of the file and empties its decoder.
bool FFReader::rewind(Demux& d, AVMediaType type, int refIndex)
{
    AVFormatContext* fc = d.fmt.get();
    bool ok = d.pristine;           // just opened and nothing read
    if (!ok && fc && refIndex >= 0 && refIndex < (int)fc->nb_streams) {
        // Aim at the earliest packet there is: that can be before the stream's official start.
        AVStream* ref = fc->streams[refIndex];
        int64_t ts = ref->start_time != kNoPts ? std::min<int64_t>(ref->start_time, 0) : 0;
        if (avformat_index_get_entries_count(ref) > 0) {
            const AVIndexEntry* first = avformat_index_get_entry(ref, 0);
            if (first && first->timestamp != kNoPts) ts = std::min(ts, first->timestamp);
        }
        ok = av_seek_frame(fc, refIndex, ts, AVSEEK_FLAG_BACKWARD) >= 0;
        if (!ok) ok = av_seek_frame(fc, refIndex, ts, 0) >= 0;
        if (ok) avcodec_flush_buffers(d.dec.get());
    }
    if (!ok) {
        Demux fresh;
        if (!openInput(info_.path, fresh.fmt, nullptr)) return false;
        if (!openStream(fresh, d.index, type, nullptr, type == AVMEDIA_TYPE_VIDEO && hwTry_)) return false;
        d = std::move(fresh);
        if (type == AVMEDIA_TYPE_VIDEO) keepOnly(d.fmt.get(), d.index, -1);
        else setupAudioStreams();
    }
    d.draining = d.ended = false;
    d.errors = 0;
    d.lastPacketTs = d.skipUntil = kNoPts;
    return true;
}

// ================================================================ frames

void FFReader::outputSize(int maxW, int maxH, int& ow, int& oh) const
{
    const int dw = info_.width, dh = info_.height;
    if (maxW < 1 || maxH < 1) {
        ow = dw;
        oh = dh;
        return;
    }
    maxW = std::min(maxW, 32768);
    maxH = std::min(maxH, 32768);
    const double s = std::min((double)maxW / dw, (double)maxH / dh);
    ow = std::max(1, std::min(maxW, (int)std::lround(dw * s)));
    oh = std::max(1, std::min(maxH, (int)std::lround(dh * s)));
}

// The time 'seconds' in the video stream's units.
int64_t FFReader::targetTick(double seconds) const
{
    const double unit = av_q2d(vTb_);
    const double lean = std::min(0.5, 0.0005 / unit) + 1e-6;
    double t = (double)vStartPts_ + seconds / unit + lean;
    if (t > 4.0e18) t = 4.0e18;
    if (t < -4.0e18) t = -4.0e18;
    return (int64_t)std::floor(t);
}

ImagePtr FFReader::cacheFind(int w, int h, int64_t target, int64_t* ptsOut, int64_t* untilOut)
{
    CacheKey k;
    k.w = w;
    k.h = h;
    k.pts = target;
    auto it = cacheIndex_.upper_bound(k);       // the first frame that starts after 'target'...
    if (it == cacheIndex_.begin()) return nullptr;
    --it;                                       // ...so this one is the last that starts at or before it
    if (it->first.w != w || it->first.h != h) return nullptr;
    CacheEntry& e = *it->second;
    if (e.until != kForever && target >= e.until) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    if (ptsOut) *ptsOut = e.key.pts;
    if (untilOut) *untilOut = e.until;
    return e.img;
}

// The first and last times that targetTick() puts at or after tick 'lo' and before tick 'hi'.
void FFReader::tickSpanSeconds(int64_t lo, int64_t hi, double* from, double* until) const
{
    const double unit = av_q2d(vTb_);
    const double lean = std::min(0.5, 0.0005 / unit) + 1e-6;
    const double inf = std::numeric_limits<double>::infinity();
    if (lo == kNoPts || (firstKnown_ && lo <= firstPts_)) *from = -inf;
    else {
        double t = ((double)lo - (double)vStartPts_ - lean) * unit;
        for (int k = 0; k < 8 && targetTick(t) < lo; ++k) t = std::nextafter(t, inf);
        for (int k = 0; k < 8 && targetTick(std::nextafter(t, -inf)) >= lo; ++k) t = std::nextafter(t, -inf);
        *from = t;
    }
    if (hi == kForever || hi == kNoPts) *until = inf;
    else {
        double t = ((double)hi - (double)vStartPts_ - lean) * unit;
        for (int k = 0; k < 8 && targetTick(t) < hi; ++k) t = std::nextafter(t, inf);
        for (int k = 0; k < 8 && targetTick(std::nextafter(t, -inf)) >= hi; ++k) t = std::nextafter(t, -inf);
        *until = t;
    }
}

void FFReader::cacheInsert(int w, int h, int64_t pts, int64_t until, const ImagePtr& img)
{
    CacheKey k;
    k.w = w;
    k.h = h;
    k.pts = pts;
    auto old = cacheIndex_.find(k);
    if (old != cacheIndex_.end()) {
        cacheBytes_ -= old->second->img->byteSize();
        lru_.erase(old->second);
        cacheIndex_.erase(old);
    }
    CacheEntry e;
    e.key = k;
    e.until = until;
    e.img = img;
    lru_.push_front(e);
    cacheIndex_[k] = lru_.begin();
    cacheBytes_ += img->byteSize();
    while (cacheBytes_ > kFrameCacheBytes && lru_.size() > 1) {
        CacheEntry& last = lru_.back();
        cacheBytes_ -= last.img->byteSize();
        cacheIndex_.erase(last.key);
        lru_.pop_back();
    }
}

void FFReader::cacheClear()
{
    lru_.clear();
    cacheIndex_.clear();
    cacheBytes_ = 0;
}

void FFReader::setDeinterlace(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    if (mode == deint_) return;
    deint_ = mode;
    cacheClear();
}

// Keeps a frame just decoded (after a seek backwards) for the frames asked for next, which will be earlier ones.
void FFReader::keepForBackwards(const AVFrame* f, int64_t pts)
{
    if (lastBackPts_ != kNoPts && lastBackPts_ < pts) {
        auto prev = back_.find(lastBackPts_);
        if (prev != back_.end()) prev->second.until = pts;
    }
    lastBackPts_ = pts;
    auto have = back_.find(pts);
    if (have != back_.end()) {
        have->second.run = backRun_;
        return;
    }
    BackFrame b;
    b.f.reset(av_frame_clone(f));
    if (!b.f) return;
    for (int k = 0; k < AV_NUM_DATA_POINTERS; ++k)
        if (b.f->buf[k]) b.bytes += b.f->buf[k]->size;
    b.run = backRun_;
    backBytes_ += b.bytes;
    back_[pts] = std::move(b);
    // Too many: those of the stretches decoded before this one go first.
    const size_t most = collecting_ ? kBackBufferBytes : kForwardKeepBytes;
    while (backBytes_ > most && back_.size() > 1) {
        auto drop = back_.begin();
        for (auto it = back_.begin(); it != back_.end(); ++it)
            if (it->second.run < drop->second.run) drop = it;
        backBytes_ -= drop->second.bytes;
        back_.erase(drop);
    }
}

// The kept frame showing at 'target', if one is known to be (null if not).
FFReader::Shown* FFReader::fromBackBuffer(int64_t target)
{
    auto it = back_.upper_bound(target);
    if (it == back_.begin()) return nullptr;
    --it;
    if (it->second.until == kNoPts || target >= it->second.until) return nullptr;
    av_frame_unref(backShown_.f.get());
    if (av_frame_ref(backShown_.f.get(), it->second.f.get()) < 0) return nullptr;
    backShown_.pts = it->first;
    backShown_.valid = true;
    backUntil_ = it->second.until;
    return &backShown_;
}

ImagePtr FFReader::videoFrameSpan(double seconds, int maxW, int maxH, double* spanFrom, double* spanUntil)
{
    *spanFrom = 1.0;
    *spanUntil = 0.0;
    if (!info_.hasVideo || !v_.dec) return nullptr;
    if (!(seconds == seconds)) seconds = 0.0;       // NaN
    int ow = 0, oh = 0;
    outputSize(maxW, maxH, ow, oh);

    int64_t target = info_.isStill ? vStartPts_ : targetTick(seconds);
    if (firstKnown_ && target < firstPts_) target = firstPts_;
    if (lastKnown_ && target > lastPts_) target = lastPts_;
    {
        int64_t hp = 0, hu = 0;
        if (ImagePtr hit = cacheFind(ow, oh, target, &hp, &hu)) {
            if (info_.isStill) stillSpan(spanFrom, spanUntil); else tickSpanSeconds(hp, lastKnown_ && hp >= lastPts_ ? kForever : hu, spanFrom, spanUntil);
            return hit;
        }
    }

    Shown* s = locateReady(target);
    if (!s) return nullptr;
    // How long it shows for: until the next frame starts, for ever if it is the last one, and otherwise.
    int64_t until = s->pts < kForever ? s->pts + 1 : kForever;
    if (s == &prev_ && cur_.valid) until = cur_.pts;
    else if (s == &cur_ && vEnded_) until = kForever;
    else if (s == &backShown_) until = backUntil_;

    // Already converted (it was found by decoding rather than by time because its end was not known)?
    CacheKey k;
    k.w = ow;
    k.h = oh;
    k.pts = s->pts;
    auto it = cacheIndex_.find(k);
    if (it != cacheIndex_.end()) {
        if (until > it->second->until) it->second->until = until;
        lru_.splice(lru_.begin(), lru_, it->second);
        if (info_.isStill) stillSpan(spanFrom, spanUntil); else tickSpanSeconds(s->pts, lastKnown_ && s->pts >= lastPts_ ? kForever : it->second->until, spanFrom, spanUntil);
        return it->second->img;
    }
    ImagePtr img = convert(s->f.get(), ow, oh);
    if (!img) return nullptr;
    cacheInsert(ow, oh, s->pts, until, img);
    if (info_.isStill) stillSpan(spanFrom, spanUntil); else tickSpanSeconds(s->pts, lastKnown_ && s->pts >= lastPts_ ? kForever : until, spanFrom, spanUntil);
    return img;
}

// (see MediaReader::harvestKept) The frames kept from the stretch just decoded that nobody has been given yet, latest first.
int FFReader::harvestKept(int maxW, int maxH, int most, const std::function<void(double, double, const ImagePtr&)>& put)
{
    if (!info_.hasVideo || info_.isStill || most <= 0) return 0;
    int ow = 0, oh = 0;
    outputSize(maxW, maxH, ow, oh);
    int done = 0;
    for (auto it = back_.rbegin(); it != back_.rend() && done < most; ++it) {
        BackFrame& b = it->second;
        if (b.shared || b.until == kNoPts || !b.f || (v_.hw && hw::isOnCard(*v_.hw, b.f.get()))) continue;
        const int64_t pts = it->first;
        b.shared = true;
        ImagePtr img;
        CacheKey k;
        k.w = ow;
        k.h = oh;
        k.pts = pts;
        auto ci = cacheIndex_.find(k);
        if (ci != cacheIndex_.end()) img = ci->second->img;
        else {
            img = convert(b.f.get(), ow, oh);
            if (img) cacheInsert(ow, oh, pts, b.until, img);
        }
        if (!img) continue;
        double from, until;
        tickSpanSeconds(pts, lastKnown_ && pts >= lastPts_ ? kForever : b.until, &from, &until);
        if (from <= until) put(from, until, img);
        ++done;
    }
    return done;
}

// (see MediaReader::videoCost) Looks only: nothing is decoded, and the cache's order is left as it is.
int FFReader::videoCost(double seconds, int maxW, int maxH)
{
    if (!info_.hasVideo || !v_.dec) return 0;
    if (!(seconds == seconds)) seconds = 0.0;
    int ow = 0, oh = 0;
    outputSize(maxW, maxH, ow, oh);
    int64_t target = info_.isStill ? vStartPts_ : targetTick(seconds);
    if (firstKnown_ && target < firstPts_) target = firstPts_;
    if (lastKnown_ && target > lastPts_) target = lastPts_;
    CacheKey k;
    k.w = ow;
    k.h = oh;
    k.pts = target;
    auto it = cacheIndex_.upper_bound(k);
    if (it != cacheIndex_.begin()) {
        --it;
        if (it->first.w == ow && it->first.h == oh && (it->second->until == kForever || target < it->second->until)) return 0;
    }
    if (!cur_.valid) return (vFresh_ && (double)(target - vStartPts_) * av_q2d(vTb_) <= kForwardDecodeSeconds) ? 1 : 2;
    const int64_t lowest = prev_.valid ? prev_.pts : cur_.pts;
    if (target < lowest) {
        auto b = back_.upper_bound(target);
        if (b != back_.begin()) {
            --b;
            if (b->second.until != kNoPts && target < b->second.until) return 1;
        }
        return 2;
    }
    if (target <= cur_.pts || vEnded_) return 1;
    // decoding on: a little way is cheap; further (up to where it would seek) costs about what a seek does
    return (double)(target - cur_.pts) * av_q2d(vTb_) <= kForwardDecodeSeconds ? 1 : 2;
}

// A deep frame (more than 8 bits) straight to float; anything else (or a frame to be turned or deinterlaced)
// the usual way, converted.
ImagePtr FFReader::videoFrameInDepth(double seconds, int maxW, int maxH, PixelDepth depth)
{
    if (depth != PixelDepth::Float || info_.bitDepth <= 8 || rotation_ != 0 || deint_ != 0 || !info_.hasVideo || !v_.dec)
        return MediaReader::videoFrameInDepth(seconds, maxW, maxH, depth);
    if (!(seconds == seconds)) seconds = 0.0;
    int ow = 0, oh = 0;
    outputSize(maxW, maxH, ow, oh);
    int64_t target = info_.isStill ? vStartPts_ : targetTick(seconds);
    if (firstKnown_ && target < firstPts_) target = firstPts_;
    if (lastKnown_ && target > lastPts_) target = lastPts_;
    Shown* s = locateReady(target);
    if (!s) return nullptr;
    ImagePtr img = convertFloat(s->f.get(), ow, oh);
    if (img) return img;
    ImagePtr b = convert(s->f.get(), ow, oh);
    return b ? b->converted(PixelDepth::Float) : nullptr;
}

// ---------------------------------------------------------------- HDR and linear frames
// How a frame's samples relate to light: most video and frames are already made for the screen (gamma-encoded SDR)
// and are shown as they are.
enum class Light { Sdr, Pq, Hlg, Linear };

// The PQ (SMPTE ST 2084) signal 0..1 as light relative to SDR white (203 cd/m2, BT.2408's reference white).
double pqToRelative(double e)
{
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    const double p = std::pow(std::max(0.0, e), 1.0 / m2);
    const double y = std::pow(std::max(0.0, p - c1) / (c2 - c3 * p), 1.0 / m1);
    return y * 10000.0 / 203.0;
}
// The HLG signal 0..1 as light relative to HLG's reference white.
double hlgToRelative(double e)
{
    const double a = 0.17883277, b = 1.0 - 4 * a, c = 0.5 - a * std::log(4 * a);
    auto shown = [&](double v) {
        v = std::max(0.0, std::min(1.0, v));
        return std::pow(v <= 0.5 ? v * v / 3.0 : (std::exp((v - c) / a) + b) / 12.0, 1.2);
    };
    return shown(e) / shown(0.75);
}
// HDR light above SDR white is folded in smoothly: unchanged up to 0.9 of it, then easing towards white.
inline double rollOff(double x)
{
    const double k = 0.9;
    return x <= k ? x : k + (1.0 - k) * (1.0 - std::exp(-(x - k) / (1.0 - k)));
}
inline double srgbEncode(double x)
{
    x = std::max(0.0, std::min(1.0, x));
    return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}

const AVFrame* FFReader::toneMapped(const AVFrame* f)
{
    Light light = Light::Sdr;
    if (f->color_trc == AVCOL_TRC_SMPTE2084) light = Light::Pq;
    else if (f->color_trc == AVCOL_TRC_ARIB_STD_B67) light = Light::Hlg;
    else if (f->color_trc == AVCOL_TRC_LINEAR || (v_.dec && v_.dec->codec_id == AV_CODEC_ID_RADIANCE_HDR)) light = Light::Linear;
    if (light == Light::Sdr) return f;
    const AVPixelFormat fmt = (AVPixelFormat)f->format;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return f;
    const int w = f->width, h = f->height;
    if (!tm_) tm_.reset(av_frame_alloc());
    if (!tm_) return f;
    if (tm_->width != w || tm_->height != h || tm_->format != AV_PIX_FMT_RGBA64LE) {
        av_frame_unref(tm_.get());
        tm_->format = AV_PIX_FMT_RGBA64LE;
        tm_->width = w;
        tm_->height = h;
        if (av_frame_get_buffer(tm_.get(), 0) < 0) return f;
    }
    const bool alpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    // the samples as R G B A 0..1, a row at a time: planar float straight from the file (values above 1 are kept),
    // anything else through swscale to 16-bit RGBA first
    const bool planarFloat = fmt == AV_PIX_FMT_GBRPF32LE || fmt == AV_PIX_FMT_GBRAPF32LE;
    if (!planarFloat) {
        if (!sws_isSupportedInput(fmt)) return f;
        const int space = swsColorspace(f->colorspace, w, h);
        const int full = (f->color_range == AVCOL_RANGE_JPEG || isJpegRangeFormat(fmt) || (desc->flags & AV_PIX_FMT_FLAG_RGB)) ? 1 : 0;
        if (!tmSws_ || tmSrcW_ != w || tmSrcH_ != h || tmSrcFmt_ != (int)fmt || tmSpace_ != space || tmFull_ != full) {
            tmSws_.reset(sws_getContext(w, h, fmt, w, h, AV_PIX_FMT_RGBA64LE, SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr));
            if (!tmSws_) return f;
            const int* coefficients = sws_getCoefficients(space);
            sws_setColorspaceDetails(tmSws_.get(), coefficients, full, coefficients, 1, 0, 1 << 16, 1 << 16);
            tmSrcW_ = w;
            tmSrcH_ = h;
            tmSrcFmt_ = (int)fmt;
            tmSpace_ = space;
            tmFull_ = full;
        }
        if (sws_scale(tmSws_.get(), f->data, f->linesize, 0, h, tm_->data, tm_->linesize) <= 0) return f;
    }
    // tables: the signal (16-bit) to relative light, and relative light 0..1 (in 16384 steps) to the sRGB signal
    if (tmLight_ != (int)light) {
        tmLight_ = (int)light;
        tmIn_.resize(65536);
        for (int i = 0; i < 65536; ++i) {
            const double e = i / 65535.0;
            tmIn_[(size_t)i] = (float)(light == Light::Pq ? pqToRelative(e) : light == Light::Hlg ? hlgToRelative(e) : e);
        }
    }
    if (tmOut_.empty()) {
        tmOut_.resize(16385);
        for (int i = 0; i <= 16384; ++i) tmOut_[(size_t)i] = (uint16_t)std::lround(srgbEncode(i / 16384.0) * 65535.0);
    }
    // BT.2020 colours to BT.709's, in linear light
    const bool wide = f->color_primaries == AVCOL_PRI_BT2020;
    const double M[3][3] = {{1.6605, -0.5876, -0.0728}, {-0.1246, 1.1329, -0.0083}, {-0.0182, -0.1006, 1.1187}};
    for (int y = 0; y < h; ++y) {
        uint16_t* d = (uint16_t*)(tm_->data[0] + (ptrdiff_t)y * tm_->linesize[0]);
        const float* pg = planarFloat ? (const float*)(f->data[0] + (ptrdiff_t)y * f->linesize[0]) : nullptr;
        const float* pb = planarFloat ? (const float*)(f->data[1] + (ptrdiff_t)y * f->linesize[1]) : nullptr;
        const float* pr = planarFloat ? (const float*)(f->data[2] + (ptrdiff_t)y * f->linesize[2]) : nullptr;
        const float* pa = planarFloat && alpha ? (const float*)(f->data[3] + (ptrdiff_t)y * f->linesize[3]) : nullptr;
        for (int x = 0; x < w; ++x, d += 4) {
            double c[3];
            if (planarFloat) {
                c[0] = std::isfinite(pr[x]) ? pr[x] : 0.0;
                c[1] = std::isfinite(pg[x]) ? pg[x] : 0.0;
                c[2] = std::isfinite(pb[x]) ? pb[x] : 0.0;
                const double a = pa ? std::max(0.0f, std::min(1.0f, std::isfinite(pa[x]) ? pa[x] : 1.0f)) : 1.0;
                d[3] = (uint16_t)std::lround(a * 65535.0);
                if (pa && a > 0.0 && a < 1.0)
                    for (int k = 0; k < 3; ++k) c[k] /= a;
            } else {
                for (int k = 0; k < 3; ++k) c[k] = tmIn_[d[k]];
                if (!alpha) d[3] = 65535;
            }
            if (wide) {
                const double r = M[0][0] * c[0] + M[0][1] * c[1] + M[0][2] * c[2];
                const double g = M[1][0] * c[0] + M[1][1] * c[1] + M[1][2] * c[2];
                const double b = M[2][0] * c[0] + M[2][1] * c[1] + M[2][2] * c[2];
                c[0] = r;
                c[1] = g;
                c[2] = b;
            }
            // folded by the brightest of the three, so that colours keep their hue
            const double m = std::max(c[0], std::max(c[1], c[2]));
            const double scale = light != Light::Linear && m > 0.9 ? rollOff(m) / m : 1.0;
            for (int k = 0; k < 3; ++k) {
                const double v = std::max(0.0, std::min(1.0, c[k] * scale));
                d[k] = tmOut_[(size_t)std::lround(v * 16384.0)];
            }
        }
    }
    tm_->color_range = AVCOL_RANGE_JPEG;
    tm_->colorspace = AVCOL_SPC_RGB;
    tm_->color_trc = AVCOL_TRC_IEC61966_2_1;
    tm_->color_primaries = AVCOL_PRI_BT709;
    tm_->flags = f->flags;
    return tm_.get();
}

ImagePtr FFReader::convertFloat(const AVFrame* f, int ow, int oh)
{
    if (!f || f->width < 1 || f->height < 1 || !f->data[0]) return nullptr;
    f = toneMapped(f);
    const AVPixelFormat fmt = (AVPixelFormat)f->format;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) || !sws_isSupportedInput(fmt)) return nullptr;
    const bool rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const int space = swsColorspace(f->colorspace, f->width, f->height);
    int full;
    if (f->color_range == AVCOL_RANGE_JPEG || isJpegRangeFormat(fmt)) full = 1;
    else if (f->color_range == AVCOL_RANGE_MPEG) full = 0;
    else full = (!rgb && desc->nb_components <= 2) ? 1 : 0;
    const double ratio = std::min((double)ow / f->width, (double)oh / f->height);
    const int flags = (ratio <= 0.5 ? SWS_AREA : SWS_BILINEAR) | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    if (!swsDeep_ || deepSrcW_ != f->width || deepSrcH_ != f->height || deepSrcFmt_ != (int)fmt || deepDstW_ != ow || deepDstH_ != oh ||
        deepSpace_ != space || deepFull_ != full || deepFlags_ != flags) {
        swsDeep_.reset(sws_getContext(f->width, f->height, fmt, ow, oh, AV_PIX_FMT_RGBA64LE, flags, nullptr, nullptr, nullptr));
        if (!swsDeep_) return nullptr;
        const int* coefficients = sws_getCoefficients(space);
        sws_setColorspaceDetails(swsDeep_.get(), coefficients, full, coefficients, 1, 0, 1 << 16, 1 << 16);
        deepSrcW_ = f->width;
        deepSrcH_ = f->height;
        deepSrcFmt_ = (int)fmt;
        deepDstW_ = ow;
        deepDstH_ = oh;
        deepSpace_ = space;
        deepFull_ = full;
        deepFlags_ = flags;
    }
    const int strideWords = FFALIGN(ow * 4, 32);
    deep_.resize((size_t)strideWords * (size_t)oh + 64);
    uint8_t* dst[4] = {(uint8_t*)deep_.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {strideWords * 2, 0, 0, 0};
    if (sws_scale(swsDeep_.get(), f->data, f->linesize, 0, f->height, dst, dstStride) <= 0) return nullptr;
    ImagePtr img = Image::make(ow, oh, PixelDepth::Float, false);
    if (!img) return nullptr;
    const bool alpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    const float k = 1.f / 65535.f;
    for (int y = 0; y < oh; ++y) {
        const uint16_t* sr = deep_.data() + (size_t)y * (size_t)strideWords;
        float* d = (float*)img->row(oh - 1 - y);
        for (int x = 0; x < ow; ++x, sr += 4, d += 4) {
            const float a = alpha ? sr[3] * k : 1.f;
            d[0] = sr[0] * k * a;
            d[1] = sr[1] * k * a;
            d[2] = sr[2] * k * a;
            d[3] = a;
        }
    }
    return img;
}

void FFReader::resetVideoState(bool atStart, int64_t seekTs)
{
    prev_.valid = cur_.valid = false;
    if (prev_.f) av_frame_unref(prev_.f.get());
    if (cur_.f) av_frame_unref(cur_.f.get());
    vEnded_ = false;
    vFresh_ = atStart;
    vExpectedPts_ = kNoPts;
    vSeekTs_ = seekTs;
    lastBackPts_ = kNoPts;
    ++backRun_;
}

// Seeks to the key frame at or before 'target'. Later attempts aim earlier and earlier, and the last one goes to the
// start of the file.
bool FFReader::seekVideo(int64_t target, int attempt)
{
    struct SeekTimer {
        const std::string& path; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~SeekTimer() { if (profile::enabled()) profile::noteSeek(path, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
    } seekTimer{info_.path};
    AVFormatContext* fc = v_.fmt.get();
    const double unit = av_q2d(vTb_);
    int64_t start = vStartPts_;
    if (firstKnown_ && firstPts_ < start) start = firstPts_;

    int64_t ts = target;
    // Formats that seek by decoding time land too late for the couple of frames that are shown just before a key
    // frame but stored after it.
    if (fc->iformat && !(fc->iformat->flags & AVFMT_SEEK_TO_PTS) && v_.st->codecpar->video_delay > 0)
        ts -= (int64_t)(3.0 / 23.0 / unit) + 1;
    if (attempt == 1) ts -= (int64_t)(1.0 / unit) + 1;
    if (attempt == 2) ts -= (int64_t)(5.0 / unit) + 1;

    bool atStart = attempt >= 3 || ts <= start;
    if (!atStart) {
        if (av_seek_frame(fc, v_.index, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(v_.dec.get());
            v_.draining = v_.ended = false;
            v_.errors = 0;
            v_.lastPacketTs = kNoPts;
        } else {
            atStart = true;         // cannot seek there
        }
    }
    if (atStart) {
        if (!rewind(v_, AVMEDIA_TYPE_VIDEO, v_.index)) return false;
        ts = start;
    }
    resetVideoState(atStart, ts);
    return true;
}

// Is the key frame before 'target' further on than where decoding has got to?
bool FFReader::worthSeekingForward(int64_t target)
{
    const double gap = (double)(target - cur_.pts) * av_q2d(vTb_);
    if (gap <= kForwardDecodeSeconds) return false;
    // If the file's index covers the target it says exactly where the key frames are.
    if (v_.lastPacketTs != kNoPts && avformat_index_get_entry_from_timestamp(v_.st, target, 0)) {
        const AVIndexEntry* key = avformat_index_get_entry_from_timestamp(v_.st, target, AVSEEK_FLAG_BACKWARD);
        if (key) return key->timestamp > v_.lastPacketTs;
    }
    return gap > kForwardSeekSeconds;
}

// Decodes the next frame into cur_ (the one that was there moves to prev_).
// 1 = done, 0 = there are no more, negative = decoding failed.
int FFReader::decodeVideoFrame()
{
    AVFrame* f;
    int64_t ts;
    for (;;) {
        f = v_.frame.get();
        int r = receiveFrame(v_, f);
        if (r > 0 && v_.hw && hw::emulationFailsDecode()) {
            av_frame_unref(f);
            r = AVERROR(EIO);
        }
        if (r < 0 && v_.hw) {                                       // the graphics card failed
            if (swapToSoftware(true)) continue;
            return r;
        }
        if (r == 0 && v_.hw && v_.hw->received == 0 && swapToSoftware(true)) continue;
        if (r <= 0) return r;
        if (v_.hw) {
            ++v_.hw->received;
            hw::markReceived(*v_.hw, f);
            if (!hw::isOnCard(*v_.hw, f)) {                         // FFmpeg could not set the card up for this stream
                av_frame_unref(f);
                if (swapToSoftware(true)) continue;
                return AVERROR(EIO);
            }
        }
        ts = f->best_effort_timestamp;
        if (ts == kNoPts) ts = f->pts;
        if (resumeAfter_ != kNoPts) {                               // frames the card had already given
            if (ts == kNoPts || ts <= resumeAfter_) {
                av_frame_unref(f);
                continue;
            }
            resumeAfter_ = kNoPts;
        }
        if (ts == kNoPts) ts = vExpectedPts_ != kNoPts ? vExpectedPts_ : vSeekTs_;      // no time stamps: count frames
        if (cur_.valid && ts <= cur_.pts) ts = cur_.pts + 1;                            // keep display order strictly increasing
        // Frames are kept for going backwards after a seek backwards.
        keepThis_ = collecting_ || v_.skipNonRefBefore == kNoPts || ts >= v_.skipNonRefBefore;
        if (keepThis_ && v_.hw && !collecting_ && decodeTarget_ != kNoPts && ts < decodeTarget_ - decodeKeepTicks_) keepThis_ = false;
        // On the graphics card a frame stays there until it is used or kept: the ones decoded on the way to a target
        // (nothing kept, nothing returned) are never copied to ordinary memory.
        if (keepThis_ && v_.hw && !hw::toSystemMemory(*v_.hw, f)) {
            av_frame_unref(f);
            if (swapToSoftware(true)) continue;
            return AVERROR(EIO);
        }
        break;
    }
    vExpectedPts_ = ts + (f->duration > 0 ? f->duration : vDefaultDuration_);

    std::swap(prev_, cur_);
    av_frame_unref(cur_.f.get());
    av_frame_move_ref(cur_.f.get(), f);
    cur_.pts = ts;
    cur_.valid = true;
    if (profile::enabled()) profile::noteDecode(info_.path, ts);
    if (keepThis_) keepForBackwards(cur_.f.get(), ts);
    else lastBackPts_ = kNoPts;
    if (vFresh_) {
        vFresh_ = false;
        if (!firstKnown_ || ts < firstPts_) firstPts_ = ts;
        firstKnown_ = true;
    }
    return 1;
}

// locate(), and the frame it found in ordinary memory (a frame decoded on the graphics card is copied here, once).
FFReader::Shown* FFReader::locateReady(int64_t target)
{
    for (int tries = 0; tries < 2; ++tries) {
        Shown* s = locate(target);
        if (!s) return nullptr;
        if (!v_.hw || !hw::isOnCard(*v_.hw, s->f.get())) return s;
        if (hw::toSystemMemory(*v_.hw, s->f.get())) return s;
        if (!swapToSoftware(false)) return nullptr;
        resetVideoState(false, target);
    }
    return nullptr;
}

// The graphics card could not decode this file (or not any further): open it again with the software decoder,
// silently, and carry on from the same frame.
bool FFReader::swapToSoftware(bool reposition)
{
    if (!v_.hw) return false;
    const int64_t at = cur_.valid ? cur_.pts : vSeekTs_;
    bool curOk = cur_.valid;
    if (prev_.valid && !hw::toSystemMemory(*v_.hw, prev_.f.get())) prev_.valid = false;
    if (cur_.valid && !hw::toSystemMemory(*v_.hw, cur_.f.get())) {
        cur_.valid = prev_.valid = false;
        curOk = false;
        lastBackPts_ = kNoPts;
        ++backRun_;
    }
    Demux fresh;
    if (!openInput(info_.path, fresh.fmt, nullptr)) return false;
    if (!openStream(fresh, v_.index, AVMEDIA_TYPE_VIDEO, nullptr, false)) return false;
    keepOnly(fresh.fmt.get(), v_.index, -1);
    fresh.skipNonRefBefore = v_.skipNonRefBefore;
    hwTry_ = false;
    v_ = std::move(fresh);
    resumeAfter_ = curOk ? cur_.pts : kNoPts;
    if (reposition && (cur_.valid || !vFresh_)) {
        int64_t start = vStartPts_;
        if (firstKnown_ && firstPts_ < start) start = firstPts_;
        int64_t ts = at;
        AVFormatContext* fc = v_.fmt.get();
        if (fc->iformat && !(fc->iformat->flags & AVFMT_SEEK_TO_PTS) && v_.st->codecpar->video_delay > 0)
            ts -= (int64_t)(3.0 / 23.0 / av_q2d(vTb_)) + 1;
        if (ts > start && av_seek_frame(fc, v_.index, ts, AVSEEK_FLAG_BACKWARD) >= 0) v_.pristine = false;
    }
    return true;
}

// Decodes forward until the frame showing at 'target' is known.
FFReader::Advance FFReader::advance(int64_t target, Shown** out)
{
    for (;;) {
        if (cur_.valid) {
            if (target < cur_.pts) {
                if (prev_.valid) {
                    *out = &prev_;
                    return kFound;
                }
                if (firstKnown_ && cur_.pts <= firstPts_) {     // before the first frame: the first frame
                    *out = &cur_;
                    return kFound;
                }
                return kTooLate;
            }
            if (vEnded_) {                                      // at or after the last frame: the last frame
                *out = &cur_;
                return kFound;
            }
        } else if (vEnded_) {
            return kTooLate;                                    // nothing at all came out
        }
        // the frame is no longer wanted (MediaReadHints::cancel): stop between two frames.
        if (mediaReadHints().cancelled()) {
            if (v_.skipNonRefBefore != kNoPts && (!cur_.valid || cur_.pts < v_.skipNonRefBefore)) prev_.valid = false;
            return kCancelled;
        }
        const int r = decodeVideoFrame();
        if (r < 0) {
            if (!cur_.valid) return kFailed;
            *out = &cur_;                                       // a damaged stretch: hold the last good frame
            return kFound;
        }
        if (r == 0) {
            vEnded_ = true;
            if (cur_.valid) {
                lastKnown_ = true;
                lastPts_ = cur_.pts;
            }
        }
    }
}

FFReader::Shown* FFReader::locate(int64_t target)
{
    // Going backwards (an earlier frame than any decoded): a frame kept from the last stretch decoded may be it.
    if (cur_.valid && target < (prev_.valid ? prev_.pts : cur_.pts)) {
        if (Shown* b = fromBackBuffer(target)) return b;
        collecting_ = true;
        lastBackPts_ = kNoPts;
        ++backRun_;
    }
    struct StopCollecting {
        FFReader* r;
        ~StopCollecting() { r->collecting_ = false; }
    } stop{this};
    for (int attempt = 0; attempt < 4; ++attempt) {
        bool seek;
        if (attempt > 0) {
            seek = true;
        } else if (!cur_.valid) {
            // Nothing decoded yet. A newly opened file is at its start, which is the right place unless the target is far in.
            seek = !vFresh_ || (double)(target - vStartPts_) * av_q2d(vTb_) > kForwardSeekSeconds;
        } else {
            const int64_t lowest = prev_.valid ? prev_.pts : cur_.pts;
            if (target < lowest) seek = true;
            else if (target > cur_.pts && !vEnded_) seek = worthSeekingForward(target);
            else seek = false;
        }
        if (seek) {
            if (vFresh_ && !cur_.valid && attempt > 0) break;   // already decoding from the very start
            if (!seekVideo(target, attempt)) return nullptr;
        }
        // On the way to a frame well ahead.
        const int64_t margin = (int64_t)std::ceil(kSkipMarginSeconds / av_q2d(vTb_));
        const int64_t from = cur_.valid ? cur_.pts : vSeekTs_;
        v_.skipNonRefBefore = (!collecting_ && !vFresh_ && (!cur_.valid || target - margin > from)) ? target - margin : kNoPts;
        decodeTarget_ = target;
        decodeKeepTicks_ = margin;
        // A quick frame (MediaReadHints::quick) for one a long way ahead.
        if (attempt == 0 && mediaReadHints().quick && !mediaReadHints().cancelled()) {
            if (!cur_.valid && seek) decodeVideoFrame();
            if (cur_.valid && target > cur_.pts && !vEnded_ && (double)(target - cur_.pts) * av_q2d(vTb_) > kQuickGapSeconds) {
                v_.skipNonRefBefore = kNoPts;
                decodeTarget_ = kNoPts;
                mediaReadHints().approximated = true;
                return &cur_;
            }
        }
        Shown* found = nullptr;
        const Advance a = advance(target, &found);
        v_.skipNonRefBefore = kNoPts;
        decodeTarget_ = kNoPts;
        if (a == kFound) return found;
        if (a == kFailed || a == kCancelled) return nullptr;
        // kTooLate: the seek landed after the target (or on nothing).
    }
    return cur_.valid ? &cur_ : nullptr;
}

ImagePtr FFReader::convert(const AVFrame* f, int ow, int oh)
{
    if (!f || f->width < 1 || f->height < 1 || !f->data[0]) return nullptr;
    f = toneMapped(f);
    const AVPixelFormat fmt = (AVPixelFormat)f->format;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) || !sws_isSupportedInput(fmt)) return nullptr;

    // swscale produces the frame as stored (top row first, not yet turned); tw x th is its size before turning.
    const bool sideways = rotation_ == 90 || rotation_ == 270;
    const int tw0 = sideways ? oh : ow, th0 = sideways ? ow : oh;
    // An interlaced frame to be deinterlaced is made at its full size first (its lines are the fields' lines
    // only before it is scaled), deinterlaced, and then scaled to the size wanted.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
    const bool frameInterlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    const bool frameTopFirst = (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
#else
    const bool frameInterlaced = f->interlaced_frame != 0;
    const bool frameTopFirst = f->top_field_first != 0;
#endif
    // Media Properties' Field order, when it says: "none" is never deinterlaced; "upper" and "lower" always are.
    const std::string& fieldNow = fieldOverride_.empty() ? info_.fieldOrder : fieldOverride_;
    const bool deinterlace = deint_ != 0 && f->height >= 4 && fieldOverride_ != "none" && (frameInterlaced || fieldNow != "none");
    const bool topFirst = fieldNow != "none" ? fieldNow == "upper" : frameTopFirst;
    const int tw = deinterlace ? f->width : tw0, th = deinterlace ? f->height : th0;

    const bool rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const int space = swsColorspace(f->colorspace, f->width, f->height);
    int full;
    if (f->color_range == AVCOL_RANGE_JPEG || isJpegRangeFormat(fmt)) full = 1;
    else if (f->color_range == AVCOL_RANGE_MPEG) full = 0;
    else full = (!rgb && desc->nb_components <= 2) ? 1 : 0;     // unlabelled grey is full range, unlabelled YUV is not
    // Area averaging for big reductions (anything else skips source pixels), bilinear otherwise.
    const double ratio = std::min((double)tw / f->width, (double)th / f->height);
    const int flags = ratio <= 0.5 ? SWS_AREA : SWS_BILINEAR;

    if (!sws_ || swsSrcW_ != f->width || swsSrcH_ != f->height || swsSrcFmt_ != (int)fmt || swsDstW_ != tw || swsDstH_ != th ||
        swsSpace_ != space || swsFull_ != full || swsFlags_ != flags) {
        sws_.reset(sws_getContext(f->width, f->height, fmt, tw, th, AV_PIX_FMT_BGRA, flags, nullptr, nullptr, nullptr));
        if (!sws_) return nullptr;
        // The source's own matrix and range; the destination is RGB, so its table and range are not used.
        const int* coefficients = sws_getCoefficients(space);
        sws_setColorspaceDetails(sws_.get(), coefficients, full, coefficients, 0, 0, 1 << 16, 1 << 16);
        swsSrcW_ = f->width;
        swsSrcH_ = f->height;
        swsSrcFmt_ = (int)fmt;
        swsDstW_ = tw;
        swsDstH_ = th;
        swsSpace_ = space;
        swsFull_ = full;
        swsFlags_ = flags;
    }

    // swscale writes whole groups of pixels and may run past the end of a row, so it gets a buffer
    // with rows padded out to 64 bytes and some slack at the end, not the Image itself.
    const int stride = FFALIGN(tw * 4, 64);
    const size_t need = (size_t)stride * (size_t)th + 64;
    if (!tmp_ || tmpBytes_ < need) {
        tmp_.reset((uint8_t*)av_malloc(need));
        tmpBytes_ = tmp_ ? need : 0;
        if (!tmp_) return nullptr;
    }
    uint8_t* dst[4] = {tmp_.get(), nullptr, nullptr, nullptr};
    int dstStride[4] = {stride, 0, 0, 0};
    if (sws_scale(sws_.get(), f->data, f->linesize, 0, f->height, dst, dstStride) <= 0) return nullptr;
    const uint8_t* src = tmp_.get();
    int srcStride = stride;
    if (deinterlace) {
        deinterlaceRows(tmp_.get(), stride, tw, th, topFirst);
        if (tw != tw0 || th != th0) {
            if (!sws2_ || sws2SrcW_ != tw || sws2SrcH_ != th || sws2DstW_ != tw0 || sws2DstH_ != th0) {
                const double r2 = std::min((double)tw0 / tw, (double)th0 / th);
                sws2_.reset(sws_getContext(tw, th, AV_PIX_FMT_BGRA, tw0, th0, AV_PIX_FMT_BGRA, r2 <= 0.5 ? SWS_AREA : SWS_BILINEAR,
                                           nullptr, nullptr, nullptr));
                if (!sws2_) return nullptr;
                sws2SrcW_ = tw;
                sws2SrcH_ = th;
                sws2DstW_ = tw0;
                sws2DstH_ = th0;
            }
            const int stride2 = FFALIGN(tw0 * 4, 64);
            const size_t need2 = (size_t)stride2 * (size_t)th0 + 64;
            if (!tmp2_ || tmp2Bytes_ < need2) {
                tmp2_.reset((uint8_t*)av_malloc(need2));
                tmp2Bytes_ = tmp2_ ? need2 : 0;
                if (!tmp2_) return nullptr;
            }
            const uint8_t* in2[4] = {tmp_.get(), nullptr, nullptr, nullptr};
            const int inStride2[4] = {stride, 0, 0, 0};
            uint8_t* out2[4] = {tmp2_.get(), nullptr, nullptr, nullptr};
            int outStride2[4] = {stride2, 0, 0, 0};
            if (sws_scale(sws2_.get(), in2, inStride2, 0, th, out2, outStride2) <= 0) return nullptr;
            src = tmp2_.get();
            srcStride = stride2;
        }
    }

    ImagePtr img = Image::make(ow, oh, PixelDepth::Byte, false);
    if (!img) return nullptr;
    const bool alpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    if (rotation_ == 0) {
        // Row y counted from the top goes to Image row oh-1-y, because Image row 0 is the bottom of the frame.
        for (int y = 0; y < oh; ++y) {
            const uint8_t* s = src + (size_t)y * (size_t)srcStride;
            uint8_t* d = img->row(oh - 1 - y);
            if (alpha) {
                for (int x = 0; x < ow; ++x) copyPremultiplied(d + 4 * x, s + 4 * x);
            } else {
                for (int x = 0; x < ow; ++x) copyOpaque(d + 4 * x, s + 4 * x);
            }
        }
    } else {
        // (x, y) is a pixel of the turned frame, counted from its top left; (sx, sy) is where it comes from.
        for (int y = 0; y < oh; ++y) {
            uint8_t* d = img->row(oh - 1 - y);
            for (int x = 0; x < ow; ++x) {
                int sx, sy;
                if (rotation_ == 90) {              // clockwise: the stored top left ends up top right
                    sx = y;
                    sy = th0 - 1 - x;
                } else if (rotation_ == 180) {
                    sx = tw0 - 1 - x;
                    sy = th0 - 1 - y;
                } else {                            // 270 clockwise
                    sx = tw0 - 1 - y;
                    sy = x;
                }
                const uint8_t* s = src + (size_t)sy * (size_t)srcStride + (size_t)sx * 4;
                if (alpha) copyPremultiplied(d + 4 * x, s);
                else copyOpaque(d + 4 * x, s);
            }
        }
    }
    return img;
}

// Deinterlaces a BGRA frame (top row first) in place.
void FFReader::deinterlaceRows(uint8_t* rows, int stride, int w, int h, bool topFirst)
{
    if (h < 3) return;
    const size_t rowBytes = (size_t)w * 4;
    std::vector<uint8_t>& copy = lineCopy_;
    copy.resize(rowBytes * (size_t)h);
    for (int y = 0; y < h; ++y) std::memcpy(copy.data() + rowBytes * (size_t)y, rows + (size_t)stride * (size_t)y, rowBytes);
    auto line = [&](int y) { return copy.data() + rowBytes * (size_t)std::min(h - 1, std::max(0, y)); };
    const int keep = topFirst ? 0 : 1;          // the parity (from the top) of the first field's lines
    for (int y = 0; y < h; ++y) {
        uint8_t* d = rows + (size_t)stride * (size_t)y;
        if (deint_ == 2) {
            if (y % 2 == keep) continue;
            // the lines above and below belong to the kept field (past the edges: the one there is)
            const uint8_t* a = line(y - 1 >= 0 ? y - 1 : y + 1);
            const uint8_t* b = line(y + 1 < h ? y + 1 : y - 1);
            for (size_t i = 0; i < rowBytes; ++i) d[i] = (uint8_t)((a[i] + b[i] + 1) >> 1);
        } else {
            const uint8_t* a = line(y - 1);
            const uint8_t* c = line(y);
            const uint8_t* b = line(y + 1);
            for (size_t i = 0; i < rowBytes; ++i) d[i] = (uint8_t)((a[i] + 2 * c[i] + b[i] + 2) >> 2);
        }
    }
}

// ================================================================ sound

// Chooses which streams the sound's demuxer keeps, and which one it seeks on.
void FFReader::setupAudioStreams()
{
    AVFormatContext* fc = a_.fmt.get();
    aRef_ = a_.index;
    if (realVideoIndex_ >= 0 && realVideoIndex_ < (int)fc->nb_streams && realVideoIndex_ != a_.index &&
        avformat_index_get_entries_count(a_.st) <= 0)
        aRef_ = realVideoIndex_;
    keepOnly(fc, a_.index, aRef_);
}

bool FFReader::ensureAudioOpen()
{
    if (a_.dec) return true;
    if (aOpenTried_ || aIndex_ < 0) return false;
    aOpenTried_ = true;
    Demux d;
    std::string e;
    if (!openInput(info_.path, d.fmt, &e) || !openStream(d, aIndex_, AVMEDIA_TYPE_AUDIO, &e)) {
        logf("media: no sound from %s: %s", pathFileName(info_.path).c_str(), e.c_str());
        return false;
    }
    a_ = std::move(d);
    setupAudioStreams();
    return true;
}

void FFReader::fifoDrop(int64_t n)
{
    if (n <= 0) return;
    fifoRead_ += (size_t)n;
    fifoPos_ += n;
    aHeadIsStart_ = false;
    if (fifoRead_ >= fifo_[0].size()) {
        fifo_[0].clear();
        fifo_[1].clear();
        fifoRead_ = 0;
    } else if (fifoRead_ >= 65536) {
        fifo_[0].erase(fifo_[0].begin(), fifo_[0].begin() + (std::ptrdiff_t)fifoRead_);
        fifo_[1].erase(fifo_[1].begin(), fifo_[1].begin() + (std::ptrdiff_t)fifoRead_);
        fifoRead_ = 0;
    }
}

void FFReader::resetAudioState()
{
    a_.draining = a_.ended = false;
    a_.errors = 0;
    a_.lastPacketTs = a_.skipUntil = kNoPts;
    fifo_[0].clear();
    fifo_[1].clear();
    fifoRead_ = 0;
    fifoPos_ = 0;
    parkResampler();                // a new one (or one used before) is set up for the first frame decoded
    aSynced_ = false;
    aEnded_ = false;
    aHeadIsStart_ = false;
    pending_.clear();
    aInPos_ = 0;
    aInRate_ = 0;
}

void FFReader::parkResampler()
{
    if (!swr_) return;
    if (swrLayout_.l.order == AV_CHANNEL_ORDER_NATIVE) {
        ParkedSwr p;
        p.fmt = swrFmt_;
        p.inRate = swrRate_;
        p.outRate = swrOutRate_;
        p.pickFirst = swrPickFirst_;
        p.pickCount = swrPickCount_;
        p.mono = swrMono_;
        p.mask = swrLayout_.l.u.mask;
        p.channels = swrLayout_.l.nb_channels;
        p.swr = std::move(swr_);
        parked_.insert(parked_.begin(), std::move(p));
        if (parked_.size() > 12) parked_.pop_back();
    }
    swr_.reset();
}

// (Re)builds the resampler if this frame is not what the current one was built for.
bool FFReader::configureResampler(const AVFrame* f, int inRate)
{
    Layout in;
    const int channels = f->ch_layout.nb_channels;
    if (channels < 1) return false;
    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&f->ch_layout)) {
        av_channel_layout_default(&in.l, channels);
    } else if (av_channel_layout_copy(&in.l, &f->ch_layout) < 0) {
        return false;
    }
    if (swr_ && swrFmt_ == f->format && swrRate_ == inRate && av_channel_layout_compare(&in.l, &swrLayout_.l) == 0) return true;

    if (swr_) flushResampler();     // what is left of the sound in the old format
    swr_.reset();
    // one used before for just this: cleared and used again
    if (in.l.order == AV_CHANNEL_ORDER_NATIVE)
        for (size_t k = 0; k < parked_.size(); ++k) {
            ParkedSwr& p = parked_[k];
            if (p.fmt != f->format || p.inRate != inRate || p.outRate != outRate_ || p.pickFirst != pickFirst_ || p.pickCount != pickCount_ ||
                p.mask != in.l.u.mask || p.channels != in.l.nb_channels)
                continue;
            SwrPtr s = std::move(p.swr);
            const bool mono = p.mono;
            parked_.erase(parked_.begin() + (std::ptrdiff_t)k);
            if (swr_init(s.get()) < 0) break;
            av_channel_layout_uninit(&swrLayout_.l);
            if (av_channel_layout_copy(&swrLayout_.l, &in.l) < 0) break;
            swr_ = std::move(s);
            swrFmt_ = f->format;
            swrRate_ = inRate;
            swrOutRate_ = outRate_;
            swrPickFirst_ = pickFirst_;
            swrPickCount_ = pickCount_;
            swrMono_ = mono;
            return true;
        }
    // Mono is resampled as mono and copied to both sides afterwards (swresample would turn it down by 3 dB); anything
    // else is mixed to stereo.
    const bool pick = pickFirst_ >= 0 && pickFirst_ < channels;
    const int pickCount = pick ? (pickCount_ >= 2 && pickFirst_ + 1 < channels ? 2 : 1) : 0;
    const bool mono = channels == 1 || pickCount == 1;
    Layout out;
    av_channel_layout_default(&out.l, mono ? 1 : 2);
    SwrContext* s = nullptr;
    int r = swr_alloc_set_opts2(&s, &out.l, AV_SAMPLE_FMT_FLTP, outRate_, &in.l, (AVSampleFormat)f->format, inRate, 0, nullptr);
    if (r < 0 || !s) return false;
    swr_.reset(s);
    if (pick && channels > 1) {
        std::vector<double> m((size_t)(mono ? 1 : 2) * (size_t)channels, 0.0);
        m[(size_t)pickFirst_] = 1.0;
        if (!mono) m[(size_t)channels + (size_t)pickFirst_ + 1] = 1.0;
        if (swr_set_matrix(s, m.data(), channels) < 0) {
            swr_.reset();
            return false;
        }
    }
    r = swr_init(s);
    if (r < 0) {
        swr_.reset();
        return false;
    }
    av_channel_layout_uninit(&swrLayout_.l);
    if (av_channel_layout_copy(&swrLayout_.l, &in.l) < 0) {
        swr_.reset();
        return false;
    }
    swrFmt_ = f->format;
    swrRate_ = inRate;
    swrOutRate_ = outRate_;
    swrPickFirst_ = pickFirst_;
    swrPickCount_ = pickCount_;
    swrMono_ = mono;
    return true;
}

// Runs 'count' samples through the resampler ('in' null: gets out what it is still holding) and
// adds what comes out to the fifo.
bool FFReader::resample(const uint8_t** in, int count)
{
    if (!swr_) return false;
    int room = in ? swr_get_out_samples(swr_.get(), count) : (int)swr_get_delay(swr_.get(), outRate_) + 64;
    if (room < 0) return false;
    room += 64;
    if ((int)conv_[0].size() < room) {
        conv_[0].resize((size_t)room);
        conv_[1].resize((size_t)room);
    }
    uint8_t* out[2] = {(uint8_t*)conv_[0].data(), (uint8_t*)conv_[1].data()};
    const int got = swr_convert(swr_.get(), out, room, in, in ? count : 0);
    if (got < 0) return false;
    if (got > 0) {
        fifo_[0].insert(fifo_[0].end(), conv_[0].begin(), conv_[0].begin() + got);
        const std::vector<float>& right = swrMono_ ? conv_[0] : conv_[1];
        fifo_[1].insert(fifo_[1].end(), right.begin(), right.begin() + got);
    }
    return true;
}

void FFReader::flushResampler()
{
    if (!swr_) return;
    for (int i = 0; i < 16; ++i) {
        const size_t before = fifo_[0].size();
        if (!resample(nullptr, 0) || fifo_[0].size() == before) break;
    }
}

// The time stamp of a decoded sound frame as a position in samples at 'rate' from the file's time zero (less VEGAS's
// AAC delay); false if it has none.
bool FFReader::framePosition(const AVFrame* f, int rate, int64_t* pos) const
{
    int64_t ts = f->best_effort_timestamp;
    if (ts == kNoPts) ts = f->pts;
    if (ts == kNoPts) return false;
    *pos = av_rescale_q(ts, a_.st->time_base, av_make_q(1, rate)) - av_rescale(originUs_, rate, 1000000) - delayAt(rate) +
           av_rescale(aStampFixNs_, rate, 1000000000);
    return true;
}

// Decodes one more frame of sound into the fifo, or notes that the sound has ended.
bool FFReader::decodeAudio()
{
    AVFrame* f = a_.frame.get();
    const int r = receiveFrame(a_, f);
    if (r < 0) return false;
    if (r == 0) {
        if (!aSynced_ && !pending_.empty() && !placePending()) return false;
        flushResampler();
        aEnded_ = true;
        if (aSynced_) {
            aEndKnown_ = true;
            aEndPos_ = fifoPos_ + fifoAvailable();
        }
        return true;
    }
    const int inRate = f->sample_rate > 0 ? f->sample_rate : a_.dec->sample_rate;
    if (f->nb_samples <= 0 || inRate <= 0 || !f->extended_data || !f->extended_data[0]) {
        av_frame_unref(f);
        return true;
    }
    if (!aSynced_) {
        FramePtr keep(av_frame_alloc());
        if (!keep) return false;
        av_frame_move_ref(keep.get(), f);
        pending_.push_back(std::move(keep));
        if ((int)pending_.size() < kAudioSyncFrames) return true;
        return placePending();
    }
    const bool ok = placeFrame(f, inRate, true);
    av_frame_unref(f);
    return ok;
}

// Puts one frame's sound after what is in the fifo.
bool FFReader::placeFrame(AVFrame* f, int inRate, bool holes)
{
    if (!configureResampler(f, inRate)) return false;
    int64_t pos = 0;
    const bool stamped = framePosition(f, inRate, &pos);
    if (inRate != aInRate_) {
        // The stream changed rate part way: carry on from where the old rate got to.
        aInPos_ = stamped ? pos : av_rescale(aInPos_, inRate, aInRate_);
        aInRate_ = inRate;
    } else if (stamped && holes) {
        // A hole in the stream (a recording that lost some sound): keep what follows in the right place.
        const int64_t hole = pos - aInPos_;
        if (hole > (int64_t)(kAudioGapSeconds * inRate) && hole < (int64_t)(kAudioMaxGapSeconds * inRate)) {
            const int64_t silence = av_rescale(hole, outRate_, inRate);
            fifo_[0].insert(fifo_[0].end(), (size_t)silence, 0.0f);
            fifo_[1].insert(fifo_[1].end(), (size_t)silence, 0.0f);
            aInPos_ = pos;
        }
    }
    if (aFromStart_ && aGridFrames_ < kAudioGridFrames && inRate == aGridRate_) {
        if (aGridFrames_ == 1) aGridOrigin_ = aInPos_;
        if (aGridFrames_ >= 1) aGrid_ = aGrid_ ? std::gcd(aGrid_, (int64_t)f->nb_samples) : (int64_t)f->nb_samples;
        ++aGridFrames_;
    }
    const bool ok = resample((const uint8_t**)f->extended_data, f->nb_samples);
    aInPos_ += f->nb_samples;
    return ok;
}

// Places the frames held since the seek: the first of them starts where most of their time stamps put it.
bool FFReader::placePending()
{
    std::vector<FramePtr> frames;
    frames.swap(pending_);
    if (frames.empty()) return true;
    const AVFrame* first = frames[0].get();
    const int rate = first->sample_rate > 0 ? first->sample_rate : a_.dec->sample_rate;
    std::vector<int64_t> implied;
    int64_t before = 0;
    for (const FramePtr& p : frames) {
        const int r = p->sample_rate > 0 ? p->sample_rate : a_.dec->sample_rate;
        if (r != rate) break;
        int64_t pos = 0;
        if (framePosition(p.get(), rate, &pos)) implied.push_back(pos - before);
        before += p->nb_samples;
    }
    int64_t pos = 0;
    if (!implied.empty()) {
        std::sort(implied.begin(), implied.end());
        pos = implied[(implied.size() - 1) / 2];
    } else {
        // No time stamps at all: assume the seek landed where it aimed (or that this is the start of the stream).
        int64_t us = aSeekUs_;
        if (aFromStart_) us = a_.st->start_time != kNoPts ? av_rescale_q(a_.st->start_time, a_.st->time_base, microseconds()) : originUs_;
        pos = av_rescale(us - originUs_, rate, 1000000) - delayAt(rate);
    }
    if (aFromStart_) {
        // learning the frames' sizes for later seeks (placeFrame), unless they have been learnt already
        if (aGridFrames_ >= kAudioGridFrames && aGridRate_ == rate) {
            aGridFrames_ = kAudioGridFrames + 1;
        } else {
            aGridFrames_ = 0;
            aGrid_ = 0;
            aGridRate_ = rate;
        }
    } else if (aGrid_ > 0 && aGridFrames_ >= 4 && rate == aGridRate_ && frames.size() > 1) {
        // how far apart the time stamps can say two places are, in samples
        const double unit = av_q2d(a_.st->time_base) * rate;
        if (unit > 1.0) {
            int64_t lastStart = pos;
            for (size_t k = 0; k + 1 < frames.size(); ++k) lastStart += frames[k]->nb_samples;
            const int64_t snapped = aGridOrigin_ + (int64_t)std::llround((double)(lastStart - aGridOrigin_) / (double)aGrid_) * aGrid_;
            const int64_t shift = std::llabs(snapped - lastStart);
            if ((double)aGrid_ >= 2.0 * unit ? shift <= (int64_t)std::ceil(unit) : ((double)aGrid_ >= unit && shift <= (int64_t)std::ceil(unit / 2)))
                pos += snapped - lastStart;
        }
    }
    aInPos_ = pos;
    aInRate_ = rate;
    fifoPos_ = av_rescale(pos, outRate_, rate);
    aSynced_ = true;
    aHeadIsStart_ = aFromStart_;
    for (FramePtr& p : frames) {
        const int r = p->sample_rate > 0 ? p->sample_rate : a_.dec->sample_rate;
        if (!placeFrame(p.get(), r, false)) return false;
    }
    return true;
}

// Decodes the first frames of the sound from the start, to learn their sizes (placePending), before the first seek
// into a stream whose time stamps are coarser than a sample.
void FFReader::learnAudioGrid()
{
    if (aGridTried_) return;
    aGridTried_ = true;
    if (av_q2d(a_.st->time_base) * (a_.dec->sample_rate > 0 ? a_.dec->sample_rate : 1) <= 1.0) return;
    if (!rewind(a_, AVMEDIA_TYPE_AUDIO, aRef_)) return;
    resetAudioState();
    aFromStart_ = true;
    while (!aEnded_ && aGridFrames_ < kAudioGridFrames)
        if (!decodeAudio()) break;
    resetAudioState();
}

// Positions the sound so that sample 'start' (at outRate_) can be decoded: seeks to a little before
// it and decodes the first frame there, which says exactly where the seek landed.
bool FFReader::seekAudio(int64_t start)
{
    struct SeekTimer {
        const std::string& path; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~SeekTimer() { if (profile::enabled()) profile::noteSeek(path, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
    } seekTimer{info_.path};
    const AVRational us = microseconds();
    int64_t wantUs = originUs_ + av_rescale(start, 1000000, outRate_) + delayUs() - (int64_t)(kAudioPrerollSeconds * 1e6);
    aPipeline_ = false;
    if (!aSkipSeek_) learnAudioGrid();

    for (int attempt = 0; attempt < 3; ++attempt) {
        AVFormatContext* fc = a_.fmt.get();
        const AVStream* ref = fc->streams[aRef_];
        int64_t startUs = a_.st->start_time != kNoPts ? av_rescale_q(a_.st->start_time, a_.st->time_base, us) : 0;
        if (ref->start_time != kNoPts) startUs = std::min(startUs, av_rescale_q(ref->start_time, ref->time_base, us));

        const bool toStart = attempt == 2 || wantUs <= startUs;
        bool skipping = false;      // going through the file from its start, throwing packets away up to the target
        if (toStart) {
            if (!rewind(a_, AVMEDIA_TYPE_AUDIO, aRef_)) return false;
        } else if (!aSkipSeek_ && av_seek_frame(fc, aRef_, av_rescale_q(wantUs, us, ref->time_base), AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(a_.dec.get());
        } else {
            // The format cannot seek (or cannot say where it landed, like a bare MP3).
            if (!rewind(a_, AVMEDIA_TYPE_AUDIO, aRef_)) return false;
            skipping = true;
        }
        resetAudioState();
        if (skipping) a_.skipUntil = av_rescale_q(wantUs, us, a_.st->time_base);
        aFromStart_ = toStart;
        aSeekUs_ = wantUs;
        while (!aSynced_ && !aEnded_) {
            if (!decodeAudio()) return false;
        }
        aPipeline_ = true;
        // Good if there is nothing there at all (past the end), if it landed at or before the wanted
        // sample, or if it started from the beginning and so could not have landed any earlier.
        if (!aSynced_ || fifoPos_ <= start || toStart || skipping) return true;
        wantUs -= 1000000;          // landed too late
    }
    return true;
}

void FFReader::setPick(int first, int count)
{
    if (first == pickFirst_ && count == pickCount_) return;
    pickFirst_ = first;
    pickCount_ = count;
    aPipeline_ = false;
}

bool FFReader::audio(int64_t start, int frames, int sampleRate, float* left, float* right)
{
    setPick(-1, 0);
    return readAudio(start, frames, sampleRate, left, right);
}

bool FFReader::audioPick(int64_t start, int frames, int sampleRate, int first, int count, float* left, float* right)
{
    if (first < 0) return audio(start, frames, sampleRate, left, right);
    if (frames > 0 && left && right) {
        std::memset(left, 0, (size_t)frames * sizeof(float));
        std::memset(right, 0, (size_t)frames * sizeof(float));
    }
    if (!info_.hasAudio || first >= info_.channels) return false;
    setPick(first, count >= 2 && first + 1 < info_.channels ? 2 : 1);
    return readAudio(start, frames, sampleRate, left, right);
}

bool FFReader::readAudio(int64_t start, int frames, int sampleRate, float* left, float* right)
{
    if (frames > 0) {
        if (left) std::memset(left, 0, (size_t)frames * sizeof(float));
        if (right) std::memset(right, 0, (size_t)frames * sizeof(float));
    }
    if (!info_.hasAudio || sampleRate < 1 || frames < 0 || !left || !right) return false;
    if (!ensureAudioOpen()) return false;
    if (sampleRate != outRate_) {
        // A different rate: positions mean something else now, so start again.
        outRate_ = sampleRate;
        aPipeline_ = false;
        aEndKnown_ = false;
    }
    if (frames == 0) return true;
    if (start > kMaxSamplePosition || start < -kMaxSamplePosition) return true;
    if (start < 0) {
        // Before time zero there is nothing, even if the audio stream itself starts a little earlier
        // (an encoder's lead-in): those samples stay silent and the rest is read from zero.
        if (start + frames <= 0) return true;
        const int lead = (int)-start;
        left += lead;
        right += lead;
        frames -= lead;
        start = 0;
    }
    if (aEndKnown_ && start >= aEndPos_) return true;                       // past the end: silence

    // Can decoding carry on from where the last call stopped?
    bool carryOn = aPipeline_ && aSynced_;
    if (carryOn) {
        if (start < fifoPos_) carryOn = aHeadIsStart_;                      // only if this is the silence before the sound starts
        else if (start > fifoPos_ + fifoAvailable() + (int64_t)(kAudioSkipSeconds * outRate_)) carryOn = aEnded_;
    }
    if (!carryOn && !seekAudio(start)) return false;

    int done = 0;
    while (done < frames) {
        const int64_t want = start + done;
        const int64_t have = fifoAvailable();
        if (have > 0) {
            if (want < fifoPos_) {                                          // before the sound starts: leave silence
                done += (int)std::min<int64_t>(frames - done, fifoPos_ - want);
            } else if (want > fifoPos_) {                                   // not there yet: throw away what comes before
                fifoDrop(std::min(have, want - fifoPos_));
            } else {
                const int n = (int)std::min<int64_t>(have, frames - done);
                std::memcpy(left + done, fifo_[0].data() + fifoRead_, (size_t)n * sizeof(float));
                std::memcpy(right + done, fifo_[1].data() + fifoRead_, (size_t)n * sizeof(float));
                fifoDrop(n);
                done += n;
            }
            continue;
        }
        if (aEnded_) break;                                                 // past the end: the rest stays silent
        if (!decodeAudio()) {
            aPipeline_ = false;
            std::memset(left, 0, (size_t)frames * sizeof(float));      // "false" promises zeroed buffers
            std::memset(right, 0, (size_t)frames * sizeof(float));
            return false;
        }
    }
    return true;
}

}  // namespace

// Decodes one more frame into the 5.1 fifo (resampled and mixed to L R C LFE Ls Rs).
bool FFReader::multiDecode(Multi& m)
{
    AVFrame* f = m.d.frame.get();
    const int r = receiveFrame(m.d, f);
    if (r <= 0) {
        m.ended = true;
        return false;
    }
    const int inRate = f->sample_rate > 0 ? f->sample_rate : m.d.dec->sample_rate;
    const int channels = f->ch_layout.nb_channels;
    if (f->nb_samples <= 0 || inRate <= 0 || channels < 1 || !f->extended_data || !f->extended_data[0]) {
        av_frame_unref(f);
        return true;
    }
    Layout in;
    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&f->ch_layout)) av_channel_layout_default(&in.l, channels);
    else if (av_channel_layout_copy(&in.l, &f->ch_layout) < 0) av_channel_layout_default(&in.l, channels);
    if (!m.swr || m.fmt != f->format || m.inRate != inRate || av_channel_layout_compare(&in.l, &m.in.l) != 0) {
        // Six channels in, six out: the file's own channels as they are (relabelled 5.1, whatever its surrounds are
        // called).
        Layout out;
        if (channels == 6) av_channel_layout_copy(&out.l, &in.l);
        else av_channel_layout_default(&out.l, 6);
        SwrContext* sc = nullptr;
        int e = swr_alloc_set_opts2(&sc, &out.l, AV_SAMPLE_FMT_FLTP, m.outRate, &in.l, (AVSampleFormat)f->format, inRate, 0, nullptr);
        m.swr.reset(sc);
        if (e >= 0 && sc) e = swr_init(sc);
        if (e < 0 || !sc) {
            m.failed = true;
            av_frame_unref(f);
            return false;
        }
        av_channel_layout_uninit(&m.in.l);
        av_channel_layout_copy(&m.in.l, &in.l);
        m.fmt = f->format;
        m.inRate = inRate;
    }
    const int room = swr_get_out_samples(m.swr.get(), f->nb_samples) + 64;
    std::vector<float> tmp[6];
    uint8_t* outp[6];
    for (int c = 0; c < 6; ++c) {
        tmp[c].resize((size_t)std::max(1, room));
        outp[c] = (uint8_t*)tmp[c].data();
    }
    const int got = swr_convert(m.swr.get(), outp, room, (const uint8_t**)f->extended_data, f->nb_samples);
    av_frame_unref(f);
    if (got < 0) {
        m.failed = true;
        return false;
    }
    for (int c = 0; c < 6; ++c) m.fifo[c].insert(m.fifo[c].end(), tmp[c].begin(), tmp[c].begin() + got);
    return true;
}

bool FFReader::audioChannels(int64_t start, int frames, int sampleRate, int channels, float* const* out)
{
    if (channels == 2) return audio(start, frames, sampleRate, out[0], out[1]);
    if (channels != 6) return MediaReader::audioChannels(start, frames, sampleRate, channels, out);
    for (int c = 0; c < 6; ++c)
        if (frames > 0) std::memset(out[c], 0, (size_t)frames * sizeof(float));
    if (!info_.hasAudio || aIndex_ < 0 || sampleRate < 1 || frames <= 0) return info_.hasAudio && frames == 0;
    if (start < 0) {
        if (start + frames <= 0) return true;
        const int lead = (int)-start;
        float* rest[6];
        for (int c = 0; c < 6; ++c) rest[c] = out[c] + lead;
        return audioChannels(0, frames - lead, sampleRate, 6, rest);
    }
    if (!multi_ || multi_->failed || multi_->outRate != sampleRate || start < multi_->pos) {
        // from the start of the file
        std::unique_ptr<Multi> m(new Multi());
        m->outRate = sampleRate;
        if (!openInput(info_.path, m->d.fmt, nullptr) || !openStream(m->d, aIndex_, AVMEDIA_TYPE_AUDIO, nullptr)) return false;
        keepOnly(m->d.fmt.get(), aIndex_, -1);
        m->pos = -delayAt(sampleRate);
        multi_ = std::move(m);
    }
    Multi& m = *multi_;
    int done = 0;
    while (done < frames) {
        const int64_t have = (int64_t)m.fifo[0].size() - (int64_t)m.read;
        const int64_t want = start + done;
        if (have > 0) {
            if (want > m.pos) {                 // not there yet: throw away what comes before
                const int64_t drop = std::min(have, want - m.pos);
                m.read += (size_t)drop;
                m.pos += drop;
            } else {
                const int n = (int)std::min<int64_t>(have, frames - done);
                for (int c = 0; c < 6; ++c) std::memcpy(out[c] + done, m.fifo[c].data() + m.read, (size_t)n * sizeof(float));
                m.read += (size_t)n;
                m.pos += n;
                done += n;
            }
            if (m.read >= 65536) {
                for (int c = 0; c < 6; ++c) m.fifo[c].erase(m.fifo[c].begin(), m.fifo[c].begin() + (std::ptrdiff_t)m.read);
                m.read = 0;
            }
            continue;
        }
        if (m.ended || !multiDecode(m)) {
            if (m.failed) return false;
            if (m.ended) break;             // past the end: the rest stays silent
        }
    }
    return true;
}

std::string mediaProjectReference(const std::string& path)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return std::string();
    std::string found;
    for (const char* key : {"comment", "description", "COMMENT", "ICMT"}) {
        const AVDictionaryEntry* e = av_dict_get(fc->metadata, key, nullptr, 0);
        if (!e || !e->value) continue;
        const std::string v = e->value;
        const size_t at = v.find("Project: ");
        if (at == std::string::npos) continue;
        const size_t end = v.find_first_of("\r\n", at);
        found = v.substr(at + 9, end == std::string::npos ? std::string::npos : end - at - 9);
        break;
    }
    avformat_close_input(&fc);
    return found;
}

std::unique_ptr<MediaReader> openMedia(const std::string& path, std::string* error)
{
    {
        std::string bytes;
        if (isAnimatedWebp(path, &bytes)) return openAnimatedWebp(path, bytes, error);
    }
    std::unique_ptr<FFReader> r(new FFReader());
    std::string e;
    if (!r->open(path, &e)) {
        if (e.empty()) e = "Cannot read " + pathFileName(path) + ".";
        if (error) *error = e;
        return nullptr;
    }
    return std::unique_ptr<MediaReader>(r.release());
}

std::string mediaBackendVersion()
{
    const unsigned v = avcodec_version();
    const char* name = av_version_info();
    return strfmt("FFmpeg %s (libavcodec %u.%u.%u)", name ? name : "?", v >> 16, (v >> 8) & 255u, v & 255u);
}

}  // namespace reno
