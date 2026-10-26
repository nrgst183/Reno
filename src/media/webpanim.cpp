// Animated WebP. FFmpeg (6.1, 7.0) reads still WebP frames but not animated ones: its WebP decoder stops at the
// animation's frames.
#include "ffmpeg_common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace reno {

namespace {

using namespace ff;

uint32_t le24(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
void put32(std::string& s, uint32_t v)
{
    for (int k = 0; k < 4; ++k) s += (char)((v >> (8 * k)) & 255);
}
void put24(std::string& s, uint32_t v)
{
    for (int k = 0; k < 3; ++k) s += (char)((v >> (8 * k)) & 255);
}

struct Frame {
    int x = 0, y = 0, w = 0, h = 0;
    int durationMs = 0;
    bool dispose = false;           // its area goes back to transparent after it has been shown
    bool blend = true;              // drawn over what is there (or else replacing it)
    std::string alph, image;        // the ALPH chunk's data
    bool lossless = false;
    int64_t startMs = 0;
};

class WebpAnimReader final : public MediaReader {
public:
    bool open(const std::string& path, const std::string& bytes, std::string* error);
    const MediaInfo& info() const override { return info_; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override;
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        (void)start;
        (void)sampleRate;
        if (frames > 0) {
            std::memset(left, 0, (size_t)frames * sizeof(float));
            std::memset(right, 0, (size_t)frames * sizeof(float));
        }
        return false;
    }

private:
    bool decodeInto(int index);     // draws frame 'index' onto canvas_
    int frameAt(double seconds) const;

    MediaInfo info_;
    std::vector<Frame> frames_;
    int cw_ = 0, ch_ = 0;
    std::vector<uint8_t> canvas_;   // straight B G R A, top row first
    int shown_ = -1;                // the frame canvas_ shows (-1: nothing drawn yet)
    bool pendingDispose_ = false;   // the frame shown is to be cleared before the next is drawn
    CodecPtr dec_;
    SwsPtr sws_;
    int swsW_ = 0, swsH_ = 0, swsFmt_ = -1;
    std::vector<uint8_t> tmp_;
    struct Cached { int index, w, h; ImagePtr img; };
    std::vector<Cached> cache_;
};

bool WebpAnimReader::open(const std::string& path, const std::string& b, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = pathFileName(path) + ": " + why;
        return false;
    };
    const uint8_t* d = (const uint8_t*)b.data();
    const size_t n = b.size();
    if (n < 30 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WEBP", 4)) return fail("not a WebP file.");
    size_t at = 12;
    bool haveCanvas = false;
    int64_t t = 0;
    while (at + 8 <= n) {
        const uint32_t size = le32(d + at + 4);
        const size_t body = at + 8;
        if (size > n - body) break;
        if (!std::memcmp(d + at, "VP8X", 4) && size >= 10) {
            cw_ = (int)le24(d + body + 4) + 1;
            ch_ = (int)le24(d + body + 7) + 1;
            haveCanvas = true;
        } else if (!std::memcmp(d + at, "ANMF", 4) && size >= 16) {
            Frame f;
            f.x = 2 * (int)le24(d + body);
            f.y = 2 * (int)le24(d + body + 3);
            f.w = (int)le24(d + body + 6) + 1;
            f.h = (int)le24(d + body + 9) + 1;
            f.durationMs = (int)le24(d + body + 12);
            f.dispose = (d[body + 15] & 1) != 0;
            f.blend = (d[body + 15] & 2) == 0;
            // its own chunks: ALPH (optional), then VP8 or VP8L
            size_t s = body + 16;
            const size_t end = body + size;
            while (s + 8 <= end) {
                const uint32_t sz = le32(d + s + 4);
                if (sz > end - s - 8) break;
                if (!std::memcmp(d + s, "ALPH", 4)) f.alph.assign((const char*)d + s + 8, sz);
                else if (!std::memcmp(d + s, "VP8 ", 4) || !std::memcmp(d + s, "VP8L", 4)) {
                    f.lossless = d[s + 3] == 'L';
                    f.image.assign((const char*)d + s, 8 + sz + (sz & 1));
                }
                s += 8 + sz + (sz & 1);
            }
            if (!f.image.empty() && f.w > 0 && f.h > 0) {
                if (f.durationMs <= 10) f.durationMs = 100;
                f.startMs = t;
                t += f.durationMs;
                frames_.push_back(f);
            }
        }
        at = body + size + (size & 1);
    }
    if (!haveCanvas || cw_ < 1 || ch_ < 1 || cw_ > 16384 || ch_ > 16384) return fail("no canvas size in the file.");
    if (frames_.empty()) return fail("no animation frames in the file.");
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WEBP);
    if (!codec) return fail("this FFmpeg has no WebP decoder.");
    dec_.reset(avcodec_alloc_context3(codec));
    if (!dec_ || avcodec_open2(dec_.get(), codec, nullptr) < 0) return fail("the WebP decoder does not start.");
    canvas_.assign((size_t)cw_ * (size_t)ch_ * 4, 0);

    info_.path = path;
    info_.hasVideo = true;
    info_.hasAlpha = true;
    info_.width = cw_;
    info_.height = ch_;
    info_.videoCodec = "webp";
    info_.isStill = frames_.size() == 1;
    if (!info_.isStill) {
        info_.duration = t / 1000.0;
        // the frame rate: that of the shortest frame when every frame lasts a whole number of them, else the average
        int shortest = frames_[0].durationMs;
        for (const Frame& f : frames_) shortest = std::min(shortest, f.durationMs);
        bool whole = true;
        for (const Frame& f : frames_) whole = whole && f.durationMs % shortest == 0;
        info_.fps = whole ? 1000.0 / shortest : frames_.size() * 1000.0 / (double)t;
        double nearest = 0;
        for (double r : {10.0, 12.0, 15.0, 20.0, 24000.0 / 1001, 24.0, 25.0, 30000.0 / 1001, 30.0, 50.0, 60000.0 / 1001, 60.0})
            if (std::fabs(info_.fps - r) <= r * 0.002 && (nearest == 0 || std::fabs(info_.fps - r) < std::fabs(info_.fps - nearest))) nearest = r;
        if (nearest > 0) info_.fps = nearest;
    }
    // the first frame must decode, or the file is no good
    if (!decodeInto(0)) return fail("the first frame cannot be decoded.");
    return true;
}

int WebpAnimReader::frameAt(double seconds) const
{
    const int64_t ms = (int64_t)std::floor(seconds * 1000.0 + 0.5);
    int k = 0;
    while (k + 1 < (int)frames_.size() && frames_[(size_t)k + 1].startMs <= ms) ++k;
    return k;
}

bool WebpAnimReader::decodeInto(int index)
{
    if (index == 0 || index <= shown_) {
        std::fill(canvas_.begin(), canvas_.end(), 0);
        shown_ = -1;
        pendingDispose_ = false;
    }
    for (int k = shown_ + 1; k <= index; ++k) {
        if (pendingDispose_ && shown_ >= 0) {
            const Frame& p = frames_[(size_t)shown_];
            for (int y = std::max(0, p.y); y < std::min(ch_, p.y + p.h); ++y)
                std::memset(canvas_.data() + ((size_t)y * cw_ + std::max(0, p.x)) * 4, 0, (size_t)std::max(0, std::min(cw_, p.x + p.w) - std::max(0, p.x)) * 4);
        }
        const Frame& f = frames_[(size_t)k];
        // the frame as a WebP file of its own
        std::string file = "RIFF";
        std::string body = "WEBP";
        if (!f.alph.empty() && !f.lossless) {
            body += "VP8X";
            put32(body, 10);
            body += (char)0x10;
            body += std::string(3, '\0');
            put24(body, (uint32_t)f.w - 1);
            put24(body, (uint32_t)f.h - 1);
            body += "ALPH";
            put32(body, (uint32_t)f.alph.size());
            body += f.alph;
            if (f.alph.size() & 1) body += '\0';
        }
        body += f.image;
        put32(file, (uint32_t)body.size());
        file += body;
        PacketPtr pkt(av_packet_alloc());
        FramePtr out(av_frame_alloc());
        if (!pkt || !out || av_new_packet(pkt.get(), (int)file.size()) < 0) return false;
        std::memcpy(pkt->data, file.data(), file.size());
        avcodec_flush_buffers(dec_.get());
        if (avcodec_send_packet(dec_.get(), pkt.get()) < 0 || avcodec_receive_frame(dec_.get(), out.get()) < 0) {
            shown_ = k;
            pendingDispose_ = f.dispose;
            continue;
        }
        // to straight B G R A at its own size
        const AVPixelFormat fmt = (AVPixelFormat)out->format;
        if (!sws_ || swsW_ != out->width || swsH_ != out->height || swsFmt_ != (int)fmt) {
            sws_.reset(sws_getContext(out->width, out->height, fmt, out->width, out->height, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr));
            if (!sws_) return false;
            const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
            sws_setColorspaceDetails(sws_.get(), coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16);
            swsW_ = out->width;
            swsH_ = out->height;
            swsFmt_ = (int)fmt;
        }
        const int stride = FFALIGN(out->width * 4, 64);
        tmp_.resize((size_t)stride * (size_t)out->height + 64);
        uint8_t* dst[4] = {tmp_.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {stride, 0, 0, 0};
        if (sws_scale(sws_.get(), out->data, out->linesize, 0, out->height, dst, dstStride) <= 0) return false;
        const bool hasAlpha = (av_pix_fmt_desc_get(fmt)->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
        for (int y = 0; y < std::min(out->height, f.h); ++y) {
            const int cy = f.y + y;
            if (cy < 0 || cy >= ch_) continue;
            const uint8_t* s = tmp_.data() + (size_t)y * stride;
            uint8_t* c = canvas_.data() + (size_t)cy * cw_ * 4;
            for (int x = 0; x < std::min(out->width, f.w); ++x, s += 4) {
                const int cx = f.x + x;
                if (cx < 0 || cx >= cw_) continue;
                uint8_t* p = c + (size_t)cx * 4;
                const int a = hasAlpha ? s[3] : 255;
                if (!f.blend || a == 255 || p[3] == 0) {
                    p[0] = s[0];
                    p[1] = s[1];
                    p[2] = s[2];
                    p[3] = (uint8_t)a;
                } else if (a > 0) {
                    // straight alpha over straight alpha
                    const int da = p[3] * (255 - a) / 255, oa = a + da;
                    for (int k2 = 0; k2 < 3; ++k2) p[k2] = (uint8_t)((s[k2] * a + p[k2] * da + oa / 2) / oa);
                    p[3] = (uint8_t)oa;
                }
            }
        }
        shown_ = k;
        pendingDispose_ = f.dispose;
    }
    return true;
}

ImagePtr WebpAnimReader::videoFrame(double seconds, int maxW, int maxH)
{
    if (!(seconds == seconds)) seconds = 0.0;
    const int index = info_.isStill ? 0 : frameAt(seconds);
    int ow = cw_, oh = ch_;
    if (maxW > 0 && maxH > 0) {
        const double s = std::min((double)std::min(maxW, 32768) / cw_, (double)std::min(maxH, 32768) / ch_);
        ow = std::max(1, std::min(maxW, (int)std::lround(cw_ * s)));
        oh = std::max(1, std::min(maxH, (int)std::lround(ch_ * s)));
    }
    for (const Cached& c : cache_)
        if (c.index == index && c.w == ow && c.h == oh) return c.img;
    if (index != shown_ && !decodeInto(index)) return nullptr;
    ImagePtr img = Image::make(ow, oh, PixelDepth::Byte, false);
    if (!img) return nullptr;
    const uint8_t* src = canvas_.data();
    int srcStride = cw_ * 4;
    std::vector<uint8_t> scaled;
    if (ow != cw_ || oh != ch_) {
        SwsPtr s(sws_getContext(cw_, ch_, AV_PIX_FMT_BGRA, ow, oh, AV_PIX_FMT_BGRA, std::min((double)ow / cw_, (double)oh / ch_) <= 0.5 ? SWS_AREA : SWS_BILINEAR,
                                nullptr, nullptr, nullptr));
        if (!s) return nullptr;
        const int stride = FFALIGN(ow * 4, 64);
        scaled.resize((size_t)stride * oh + 64);
        const uint8_t* in[4] = {canvas_.data(), nullptr, nullptr, nullptr};
        const int inStride[4] = {cw_ * 4, 0, 0, 0};
        uint8_t* outp[4] = {scaled.data(), nullptr, nullptr, nullptr};
        int outStride[4] = {stride, 0, 0, 0};
        if (sws_scale(s.get(), in, inStride, 0, ch_, outp, outStride) <= 0) return nullptr;
        src = scaled.data();
        srcStride = stride;
    }
    for (int y = 0; y < oh; ++y) {
        const uint8_t* s = src + (size_t)y * srcStride;
        uint8_t* d = img->row(oh - 1 - y);
        for (int x = 0; x < ow; ++x, s += 4, d += 4) {
            const unsigned a = s[3];
            d[0] = (uint8_t)((s[0] * a + 127) / 255);
            d[1] = (uint8_t)((s[1] * a + 127) / 255);
            d[2] = (uint8_t)((s[2] * a + 127) / 255);
            d[3] = (uint8_t)a;
        }
    }
    cache_.push_back({index, ow, oh, img});
    if (cache_.size() > 64) cache_.erase(cache_.begin());
    return img;
}

}  // namespace

// Whether 'path' is an animated WebP (VP8X with the animation flag); its bytes are read into *bytes when it is.
bool isAnimatedWebp(const std::string& path, std::string* bytes)
{
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(widen(path).c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    uint8_t head[21] = {0};
    const bool animated = std::fread(head, 1, sizeof(head), f) == sizeof(head) && !std::memcmp(head, "RIFF", 4) && !std::memcmp(head + 8, "WEBP", 4) &&
                          !std::memcmp(head + 12, "VP8X", 4) && (head[20] & 0x02);
    if (animated && bytes) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0 && size < (1l << 30)) {
            bytes->resize((size_t)size);
            bytes->resize(std::fread(&(*bytes)[0], 1, (size_t)size, f));
        }
    }
    std::fclose(f);
    return animated;
}

std::unique_ptr<MediaReader> openAnimatedWebp(const std::string& path, const std::string& bytes, std::string* error)
{
    std::unique_ptr<WebpAnimReader> r(new WebpAnimReader());
    if (!r->open(path, bytes, error)) return nullptr;
    return std::unique_ptr<MediaReader>(r.release());
}

}  // namespace reno
