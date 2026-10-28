// Private to ffmpeg_reader.cpp and ffmpeg_writer.cpp: the FFmpeg headers, owning pointers for the FFmpeg objects, and
// the few decisions the reader and the writer have to make the same way.
#pragma once
#include "media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/spherical.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

namespace reno {
namespace ff {

// ---------------------------------------------------------------- owning pointers
struct InputDeleter  { void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); } };
struct CodecDeleter  { void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); } };
struct FrameDeleter  { void operator()(AVFrame* p) const { if (p) av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { if (p) av_packet_free(&p); } };
struct SwsDeleter    { void operator()(SwsContext* p) const { if (p) sws_freeContext(p); } };
struct SwrDeleter    { void operator()(SwrContext* p) const { if (p) swr_free(&p); } };
struct FifoDeleter   { void operator()(AVAudioFifo* p) const { if (p) av_audio_fifo_free(p); } };
struct MemDeleter    { void operator()(uint8_t* p) const { av_free(p); } };
// An output file: closes the file (if the format has one) and frees the context.
struct OutputDeleter {
    void operator()(AVFormatContext* p) const
    {
        if (!p) return;
        if (p->pb && p->oformat && !(p->oformat->flags & AVFMT_NOFILE)) avio_closep(&p->pb);
        avformat_free_context(p);
    }
};

using InputPtr  = std::unique_ptr<AVFormatContext, InputDeleter>;
using OutputPtr = std::unique_ptr<AVFormatContext, OutputDeleter>;
using CodecPtr  = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr  = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr    = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr    = std::unique_ptr<SwrContext, SwrDeleter>;
using FifoPtr   = std::unique_ptr<AVAudioFifo, FifoDeleter>;
using MemPtr    = std::unique_ptr<uint8_t, MemDeleter>;

struct Dict {                       // options handed to avcodec_open2 / avformat_write_header
    AVDictionary* d = nullptr;
    Dict() {}
    ~Dict() { av_dict_free(&d); }
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;
    void set(const char* key, const char* value) { av_dict_set(&d, key, value, 0); }
};

struct Layout {                     // an AVChannelLayout that cleans up after itself
    AVChannelLayout l;
    Layout() { l = AVChannelLayout(); }
    ~Layout() { av_channel_layout_uninit(&l); }
    Layout(const Layout&) = delete;
    Layout& operator=(const Layout&) = delete;
};

// ---------------------------------------------------------------- small helpers
inline std::string errText(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(err, buf, sizeof(buf)) < 0 || !buf[0]) return strfmt("FFmpeg error %d", err);
    return buf;
}

inline AVRational microseconds() { return av_make_q(1, 1000000); }

// The reader and the writer must agree on which frames count as high definition, because that
// decides the colour matrix whenever a file does not say (BT.709 for HD, BT.601 otherwise).
inline bool isHD(int w, int h) { return w >= 1280 || h > 576; }

// The SWS_CS_* number (for sws_getCoefficients) to use for a frame of this size whose file says 'space'.
inline int swsColorspace(int space, int w, int h)
{
    switch (space) {
        case AVCOL_SPC_BT709:      return SWS_CS_ITU709;
        case AVCOL_SPC_FCC:        return SWS_CS_FCC;
        case AVCOL_SPC_BT470BG:    return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE170M:  return SWS_CS_SMPTE170M;
        case AVCOL_SPC_SMPTE240M:  return SWS_CS_SMPTE240M;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:  return SWS_CS_BT2020;
        default:                   return isHD(w, h) ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

inline bool isJpegRangeFormat(int pixFmt)
{
    return pixFmt == AV_PIX_FMT_YUVJ420P || pixFmt == AV_PIX_FMT_YUVJ422P || pixFmt == AV_PIX_FMT_YUVJ444P ||
           pixFmt == AV_PIX_FMT_YUVJ440P || pixFmt == AV_PIX_FMT_YUVJ411P;
}

}  // namespace ff

// Animated WebP, which FFmpeg's own WebP decoder cannot do (webpanim.cpp). isAnimatedWebp reads the
// file into *bytes when it is one.
bool isAnimatedWebp(const std::string& path, std::string* bytes);
std::unique_ptr<MediaReader> openAnimatedWebp(const std::string& path, const std::string& bytes, std::string* error);

// HEIC / HEIF / AVIF stills, grids and alpha included (heif.cpp). isHeifStill reads the file into *bytes when it has a
// HEIF brand; openHeifStill returns null (and leaves *error empty) when the file is not one it can handle.
bool isHeifStill(const std::string& path, std::string* bytes);
std::unique_ptr<MediaReader> openHeifStill(const std::string& path, const std::string& bytes, std::string* error);

}  // namespace reno
