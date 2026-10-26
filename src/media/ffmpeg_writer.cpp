// Writing media files with FFmpeg: reno::createMediaWriter().
#include "ffmpeg_common.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace {

using namespace ff;

// Sound is handed to the encoder in pieces of this many samples when the encoder does not care (PCM).
const int kFreeAudioFrame = 1024;
// And is run through the resampler in pieces of at most this many.
const int kAudioChunk = 8192;

void setError(std::string* error, const std::string& text)
{
    if (error) *error = text;
}

bool oneOf(const std::string& s, const char* const* list)
{
    for (; *list; ++list)
        if (s == *list) return true;
    return false;
}

bool isMfEncoder(const AVCodec* codec)
{
    const std::string n = codec->name;
    return n.size() > 3 && n.compare(n.size() - 3, 3, "_mf") == 0;
}

// ---------------------------------------------------------------- what an encoder supports
std::vector<AVPixelFormat> pixelFormats(const AVCodec* codec)
{
    std::vector<AVPixelFormat> v;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* list = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) >= 0 && list)
        v.assign((const AVPixelFormat*)list, (const AVPixelFormat*)list + n);
#else
    if (codec->pix_fmts)
        for (const AVPixelFormat* p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) v.push_back(*p);
#endif
    return v;
}

std::vector<AVSampleFormat> sampleFormats(const AVCodec* codec)
{
    std::vector<AVSampleFormat> v;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* list = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &list, &n) >= 0 && list)
        v.assign((const AVSampleFormat*)list, (const AVSampleFormat*)list + n);
#else
    if (codec->sample_fmts)
        for (const AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) v.push_back(*p);
#endif
    return v;
}

std::vector<int> sampleRates(const AVCodec* codec)
{
    std::vector<int> v;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* list = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &list, &n) >= 0 && list)
        v.assign((const int*)list, (const int*)list + n);
#else
    if (codec->supported_samplerates)
        for (const int* p = codec->supported_samplerates; *p != 0; ++p) v.push_back(*p);
#endif
    return v;
}

// Sets 'out' to 'channels' (1, 2 or 6) if the encoder takes that, otherwise to the first layout it does take.
bool chooseChannelLayout(const AVCodec* codec, AVChannelLayout* out, int channels)
{
    const AVChannelLayout* list = nullptr;
    int n = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* p = nullptr;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &p, &n) >= 0 && p)
        list = (const AVChannelLayout*)p;
    else
        n = 0;
#else
    list = codec->ch_layouts;
    if (list)
        while (list[n].nb_channels > 0) ++n;
#endif
    av_channel_layout_uninit(out);
    if (list && n > 0) {
        for (int i = 0; i < n; ++i)
            if (list[i].nb_channels == channels) return av_channel_layout_copy(out, &list[i]) >= 0;
        if (channels > 2)
            for (int i = 0; i < n; ++i)
                if (list[i].nb_channels == 2) return av_channel_layout_copy(out, &list[i]) >= 0;
        return av_channel_layout_copy(out, &list[0]) >= 0;
    }
    av_channel_layout_default(out, channels);
    return true;
}

bool hasAlpha(AVPixelFormat f)
{
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
    return d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
}

// Frames in these formats are made by the writer itself, not by swscale.
bool madeHere(AVPixelFormat f) { return f == AV_PIX_FMT_PAL8 || f == AV_PIX_FMT_GBRPF32LE || f == AV_PIX_FMT_GBRAPF32LE; }

AVPixelFormat choosePixelFormat(const AVCodec* codec, const EncodeSettings& s)
{
    const std::vector<AVPixelFormat> have = pixelFormats(codec);
    // FFmpeg's raw video encoder lists no formats (it takes any): uncompressed AVI is RGB on Windows, as VEGAS writes it
    if (have.empty() && codec->id == AV_CODEC_ID_RAWVIDEO) return s.alpha ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR24;
    if (have.empty()) return AV_PIX_FMT_YUV420P;
    std::vector<AVPixelFormat> want;
    const std::string name = codec->name;
    if (s.alpha) {
        // formats that keep the transparency, where the encoder has one
        const AVPixelFormat a[] = {AV_PIX_FMT_YUVA444P10LE, AV_PIX_FMT_BGRA, AV_PIX_FMT_RGBA, AV_PIX_FMT_ARGB, AV_PIX_FMT_YUVA420P};
        for (AVPixelFormat f : a) want.push_back(f);
    }
    if (codec->id == AV_CODEC_ID_PRORES) {
        // 4444 keeps colour at full size; the others are 4:2:2, all 10-bit
        if (s.profile == "4444" || s.profile == "4444xq") want.push_back(AV_PIX_FMT_YUV444P10LE);
        want.push_back(AV_PIX_FMT_YUV422P10LE);
    }
    if ((codec->id == AV_CODEC_ID_HEVC || codec->id == AV_CODEC_ID_H264) && (s.profile == "main10" || s.profile == "high10")) {
        want.push_back(AV_PIX_FMT_YUV420P10LE);
        want.push_back(AV_PIX_FMT_P010LE);
    }
    if (codec->id == AV_CODEC_ID_DNXHD) want.push_back(AV_PIX_FMT_YUV422P);
    // DV25: NTSC 4:1:1, PAL 4:2:0; DVCPRO50 and DVCPRO HD (profile "dv50", "dv100"): 4:2:2
    if (codec->id == AV_CODEC_ID_DVVIDEO) {
        if (s.profile == "dv50" || s.profile == "dv100") want.push_back(AV_PIX_FMT_YUV422P);
        want.push_back(s.height == 480 ? AV_PIX_FMT_YUV411P : AV_PIX_FMT_YUV420P);
    }
    // AVC-Intra 100 is 4:2:2 10-bit (profile "high422"; AVC-Intra 50 is 4:2:0 10-bit: "high10")
    if (codec->id == AV_CODEC_ID_H264 && s.profile == "high422") want.push_back(AV_PIX_FMT_YUV422P10LE);
    if (isMfEncoder(codec) || name.find("_qsv") != std::string::npos) want.push_back(AV_PIX_FMT_NV12);     // what Media Foundation and Quick Sync work in
    if (codec->id == AV_CODEC_ID_MJPEG || codec->id == AV_CODEC_ID_LJPEG) want.push_back(AV_PIX_FMT_YUVJ420P);   // JPEG is full range
    if (codec->id == AV_CODEC_ID_RAWVIDEO) want.push_back(s.alpha ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR24);
    // GIF: 256 colours of its own per frame.
    if (codec->id == AV_CODEC_ID_GIF) want.insert(want.begin(), AV_PIX_FMT_PAL8);
    if (codec->id == AV_CODEC_ID_EXR) want.insert(want.begin(), s.alpha ? AV_PIX_FMT_GBRAPF32LE : AV_PIX_FMT_GBRPF32LE);
    if (codec->id == AV_CODEC_ID_FFV1) want.push_back(AV_PIX_FMT_YUV444P);
    if (codec->id == AV_CODEC_ID_QTRLE) want.push_back(AV_PIX_FMT_RGB24);
    const AVPixelFormat usual[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12,        AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV422P,
                                   AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV444P10LE,
                                   AV_PIX_FMT_RGB24,   AV_PIX_FMT_BGR24,       AV_PIX_FMT_BGR0,     AV_PIX_FMT_RGB0,
                                   AV_PIX_FMT_BGRA,    AV_PIX_FMT_RGBA};
    want.insert(want.end(), usual, usual + sizeof(usual) / sizeof(usual[0]));
    for (AVPixelFormat w : want)
        if (std::find(have.begin(), have.end(), w) != have.end() && (sws_isSupportedOutput(w) || madeHere(w))) return w;
    for (AVPixelFormat h : have)
        if (sws_isSupportedOutput(h)) return h;
    return have[0];
}

AVSampleFormat chooseSampleFormat(const AVCodec* codec)
{
    const std::vector<AVSampleFormat> have = sampleFormats(codec);
    if (have.empty()) return AV_SAMPLE_FMT_FLTP;
    const AVSampleFormat want[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16,
                                   AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_DBLP, AV_SAMPLE_FMT_DBL};
    for (AVSampleFormat w : want)
        if (std::find(have.begin(), have.end(), w) != have.end()) return w;
    return have[0];
}

int chooseSampleRate(const AVCodec* codec, int wanted)
{
    const std::vector<int> have = sampleRates(codec);
    if (have.empty()) return wanted;
    int best = have[0];
    for (int r : have) {
        if (r == wanted) return r;
        if (std::abs(r - wanted) < std::abs(best - wanted)) best = r;
    }
    return best;
}

// ---------------------------------------------------------------- which encoders to try
struct Candidate {
    std::string name;
    const AVCodec* codec;       // null if this FFmpeg does not have it
};

void addByName(std::vector<Candidate>& out, const char* const* names, const AVOutputFormat* format, bool checkContainer)
{
    for (; *names; ++names) {
        Candidate c;
        c.name = *names;
        c.codec = avcodec_find_encoder_by_name(*names);
        if (c.codec && !av_codec_is_encoder(c.codec)) c.codec = nullptr;
        // For containers without a list of their own, only offer what the container says it can hold.
        if (c.codec && checkContainer && avformat_query_codec(format, c.codec->id, FF_COMPLIANCE_NORMAL) != 1) continue;
        out.push_back(c);
    }
}

void addContainerDefault(std::vector<Candidate>& out, const AVOutputFormat* format, const std::string& path, AVMediaType type)
{
    const AVCodecID id = av_guess_codec(format, nullptr, path.c_str(), nullptr, type);
    if (id == AV_CODEC_ID_NONE) return;
    const AVCodec* codec = avcodec_find_encoder(id);
    if (!codec) return;
    for (const Candidate& c : out)
        if (c.codec == codec) return;
    Candidate c;
    c.name = codec->name;
    c.codec = codec;
    out.push_back(c);
}

// The encoders of a kind of frame compression (EncodeSettings::videoFamily), by encode mode.
struct FamilyEncoders {
    const char* family;
    const char* const* cpu;     // software encoders, best first
    const char* nvenc;
    const char* qsv;
    const char* amf;
    const char* mf;
};
const char* const kH264Cpu[] = {"libx264", "libopenh264", nullptr};
const char* const kHevcCpu[] = {"libx265", nullptr};
const char* const kProres[] = {"prores_ks", "prores", nullptr};
const char* const kDnxhd[] = {"dnxhd", nullptr};
const char* const kMpeg2[] = {"mpeg2video", nullptr};
const char* const kDv[] = {"dvvideo", nullptr};
const char* const kMpeg4[] = {"mpeg4", nullptr};
const char* const kMjpeg[] = {"mjpeg", nullptr};
const char* const kWmv[] = {"wmv2", "wmv1", nullptr};
const char* const kVp9[] = {"libvpx-vp9", "libvpx", nullptr};
const char* const kRaw[] = {"rawvideo", nullptr};
const char* const kUt[] = {"utvideo", nullptr};
const char* const kPng[] = {"png", nullptr};
const char* const kTiff[] = {"tiff", nullptr};
const char* const kBmp[] = {"bmp", nullptr};
const char* const kVp8[] = {"libvpx", nullptr};
const char* const kAv1[] = {"libsvtav1", "libaom-av1", "librav1e", nullptr};
const char* const kTheora[] = {"libtheora", nullptr};
const char* const kGif[] = {"gif", nullptr};
const char* const kApng[] = {"apng", nullptr};
const char* const kQtrle[] = {"qtrle", nullptr};
const char* const kFfv1[] = {"ffv1", nullptr};
const char* const kExr[] = {"exr", nullptr};
const char* const kTga[] = {"targa", nullptr};
const char* const kWebp[] = {"libwebp", nullptr};
const FamilyEncoders kFamilies[] = {
    {"h264", kH264Cpu, "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"},
    {"hevc", kHevcCpu, "hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf"},
    {"prores", kProres, nullptr, nullptr, nullptr, nullptr},
    {"dnxhd", kDnxhd, nullptr, nullptr, nullptr, nullptr},
    {"mpeg2", kMpeg2, nullptr, "mpeg2_qsv", nullptr, nullptr},
    {"dv", kDv, nullptr, nullptr, nullptr, nullptr},
    {"mpeg4", kMpeg4, nullptr, nullptr, nullptr, nullptr},
    {"mjpeg", kMjpeg, nullptr, "mjpeg_qsv", nullptr, nullptr},
    {"jpeg", kMjpeg, nullptr, nullptr, nullptr, nullptr},
    {"wmv", kWmv, nullptr, nullptr, nullptr, nullptr},
    {"vp9", kVp9, "vp9_nvenc", "vp9_qsv", nullptr, nullptr},
    {"rawvideo", kRaw, nullptr, nullptr, nullptr, nullptr},
    {"utvideo", kUt, nullptr, nullptr, nullptr, nullptr},
    {"png", kPng, nullptr, nullptr, nullptr, nullptr},
    {"tiff", kTiff, nullptr, nullptr, nullptr, nullptr},
    {"bmp", kBmp, nullptr, nullptr, nullptr, nullptr},
    {"vp8", kVp8, nullptr, nullptr, nullptr, nullptr},
    {"av1", kAv1, "av1_nvenc", "av1_qsv", "av1_amf", nullptr},
    {"theora", kTheora, nullptr, nullptr, nullptr, nullptr},
    {"gif", kGif, nullptr, nullptr, nullptr, nullptr},
    {"apng", kApng, nullptr, nullptr, nullptr, nullptr},
    {"qtrle", kQtrle, nullptr, nullptr, nullptr, nullptr},
    {"ffv1", kFfv1, nullptr, nullptr, nullptr, nullptr},
    {"exr", kExr, nullptr, nullptr, nullptr, nullptr},
    {"tga", kTga, nullptr, nullptr, nullptr, nullptr},
    {"webp", kWebp, nullptr, nullptr, nullptr, nullptr},
};

const FamilyEncoders* familyOf(const std::string& family)
{
    for (const FamilyEncoders& f : kFamilies)
        if (family == f.family) return &f;
    return nullptr;
}

static std::atomic<bool> gQsvAllowed{true};
}  // namespace
void setQsvEncodingAllowed(bool on) { gQsvAllowed.store(on); }
bool qsvEncodingAllowed() { return gQsvAllowed.load(); }
namespace {

// The encoders to try for a family: the encode mode's first, then the usual order (software, Media Foundation,
// then the graphics cards'), each once.
std::vector<std::string> familyOrder(const FamilyEncoders& f, const std::string& mode)
{
    std::vector<std::string> v;
    auto add = [&v](const char* n) {
        if (n && std::find(v.begin(), v.end(), std::string(n)) == v.end()) v.push_back(n);
    };
    if (mode == "nvenc") add(f.nvenc);
    else if (mode == "qsv") { if (gQsvAllowed.load()) add(f.qsv); }
    else if (mode == "amf") add(f.amf);
    else if (mode == "mf") add(f.mf);
    for (const char* const* c = f.cpu; *c; ++c) add(*c);
    if (mode != "cpu" || v.empty()) {
        add(f.mf);
        add(f.nvenc);
        if (gQsvAllowed.load()) add(f.qsv);
        add(f.amf);
    }
    return v;
}

std::vector<Candidate> videoCandidates(const std::string& ext, const AVOutputFormat* format, const std::string& path, const EncodeSettings& s)
{
    std::vector<Candidate> v;
    if (const FamilyEncoders* fam = familyOf(s.videoFamily)) {
        for (const std::string& n : familyOrder(*fam, s.encodeMode)) {
            Candidate c;
            c.name = n;
            c.codec = avcodec_find_encoder_by_name(n.c_str());
            if (c.codec && !av_codec_is_encoder(c.codec)) c.codec = nullptr;
            v.push_back(c);
        }
        return v;
    }
    static const char* const h264ish[] = {"libx264", "h264_mf", "libopenh264", "mpeg4", nullptr};
    static const char* const webm[] = {"libvpx-vp9", "libvpx", "libsvtav1", "libaom-av1", nullptr};
    static const char* const avi[] = {"mpeg4", "mjpeg", nullptr};
    static const char* const gif[] = {"gif", nullptr};
    static const char* const ogv[] = {"libtheora", nullptr};
    static const char* const h264Containers[] = {"mp4", "m4v", "mov", "mkv", nullptr};
    if (ext == "webm") addByName(v, webm, format, false);
    else if (ext == "gif") addByName(v, gif, format, false);
    else if (ext == "ogv") addByName(v, ogv, format, false);
    else if (ext == "avi") addByName(v, avi, format, false);
    else if (oneOf(ext, h264Containers)) {
        // an encode mode asks for that graphics card's H.264 encoder first
        if (s.encodeMode != "auto" && !s.encodeMode.empty())
            for (const std::string& n : familyOrder(kFamilies[0], s.encodeMode)) {
                const char* one[] = {n.c_str(), nullptr};
                addByName(v, one, format, false);
            }
        addByName(v, h264ish, format, false);
    } else {
        // Anything else: H.264 if the container holds it, then whatever the container normally holds.
        const std::string name = format->name ? format->name : "";
        if (name != "image2" && name != "image2pipe") addByName(v, h264ish, format, true);
        addContainerDefault(v, format, path, AVMEDIA_TYPE_VIDEO);
    }
    std::vector<Candidate> once;
    for (const Candidate& c : v) {
        bool seen = false;
        for (const Candidate& o : once) seen = seen || o.name == c.name;
        if (!seen) once.push_back(c);
    }
    return once;
}

std::vector<Candidate> audioCandidates(const std::string& ext, const AVOutputFormat* format, const std::string& path, const EncodeSettings& s)
{
    static const char* const mp4[] = {"aac", "aac_mf", "libmp3lame", "ac3", nullptr};
    static const char* const mov[] = {"aac", "aac_mf", "libmp3lame", "pcm_s16le", nullptr};
    static const char* const mkv[] = {"aac", "aac_mf", "libmp3lame", "ac3", "flac", "pcm_s16le", nullptr};
    static const char* const webm[] = {"libopus", "libvorbis", "opus", "vorbis", nullptr};
    static const char* const avi[] = {"libmp3lame", "ac3", "pcm_s16le", nullptr};
    static const char* const mp3[] = {"libmp3lame", "mp3_mf", nullptr};
    static const char* const flac[] = {"flac", nullptr};
    static const char* const ogg[] = {"libvorbis", "vorbis", "libopus", "opus", "flac", nullptr};
    static const char* const opus[] = {"libopus", "opus", nullptr};
    static const char* const aac[] = {"aac", "aac_mf", nullptr};
    static const char* const vorbis[] = {"libvorbis", "vorbis", nullptr};
    static const char* const alac[] = {"alac", nullptr};
    static const char* const caf[] = {"alac", "pcm_s16le", nullptr};
    static const char* const wma[] = {"wmav2", "wmav1", nullptr};
    static const char* const ac3[] = {"ac3", nullptr};
    static const char* const other[] = {"aac", "aac_mf", "libmp3lame", nullptr};
    static const char* const mp4Exts[] = {"mp4", "m4v", "m4a", "3gp", nullptr};
    static const char* const mkvExts[] = {"mkv", "mka", nullptr};
    static const char* const wavExts[] = {"wav", "w64", nullptr};
    static const char* const oggExts[] = {"ogg", "oga", "ogv", nullptr};
    static const char* const aiffExts[] = {"aif", "aiff", "aifc", nullptr};
    // PCM at the depth asked for
    const char* pcm = s.audioBits >= 32 ? "pcm_f32le" : s.audioBits >= 24 ? "pcm_s24le" : "pcm_s16le";
    if (s.audioBits >= 32 && (ext == "avi" || ext == "mxf")) pcm = "pcm_s24le";
    // AIFF and QuickTime keep PCM big-endian (AIFF-C for floating point)
    if (oneOf(ext, aiffExts)) pcm = s.audioBits >= 32 ? "pcm_f32be" : s.audioBits >= 24 ? "pcm_s24be" : "pcm_s16be";
    const char* pcmList[] = {pcm, oneOf(ext, aiffExts) ? "pcm_s16be" : "pcm_s16le", nullptr};
    std::vector<Candidate> v;
    const std::string& fam = s.audioFamily;
    if (fam == "pcm") addByName(v, pcmList, format, false);
    else if (fam == "aac") addByName(v, aac, format, false);
    else if (fam == "mp3") addByName(v, mp3, format, false);
    else if (fam == "flac") addByName(v, flac, format, false);
    else if (fam == "vorbis") addByName(v, vorbis, format, false);
    else if (fam == "opus") addByName(v, opus, format, false);
    else if (fam == "wma") addByName(v, wma, format, false);
    else if (fam == "ac3") addByName(v, ac3, format, false);
    else if (fam == "alac") addByName(v, alac, format, false);
    else if (oneOf(ext, mp4Exts)) addByName(v, mp4, format, false);
    else if (ext == "mov") addByName(v, mov, format, false);
    else if (oneOf(ext, mkvExts)) addByName(v, mkv, format, false);
    else if (ext == "webm") addByName(v, webm, format, false);
    else if (ext == "avi") addByName(v, avi, format, false);
    else if (oneOf(ext, wavExts) || oneOf(ext, aiffExts)) addByName(v, pcmList, format, false);
    else if (ext == "caf") addByName(v, caf, format, false);
    else if (ext == "mp3") addByName(v, mp3, format, false);
    else if (ext == "flac") addByName(v, flac, format, false);
    else if (oneOf(ext, oggExts)) addByName(v, ogg, format, false);
    else if (ext == "opus") addByName(v, opus, format, false);
    else if (ext == "wma" || ext == "wmv" || ext == "asf") addByName(v, wma, format, false);
    else {
        addByName(v, other, format, true);
        addContainerDefault(v, format, path, AVMEDIA_TYPE_AUDIO);
    }
    return v;
}

// ---------------------------------------------------------------- the writer
class FFWriter final : public MediaWriter {
public:
    FFWriter() {}
    ~FFWriter() override { release(); }
    bool open(const EncodeSettings& settings, std::string* error);

    bool writeVideo(const Image& frame) override;
    bool writeAudio(const float* left, const float* right, int frames) override;
    bool writeAudioChannels(const float* const* ch, int channels, int frames) override;
    int inChannels_ = 2;            // what the caller gives: 2, or 6 (5.1)
    bool finish(std::string* error) override;
    std::string describe() const override { return description_; }

private:
    bool fail(const std::string& what, int err = 0);
    bool openVideoEncoder(const AVCodec* codec, std::string& why);
    bool openAudioEncoder(const AVCodec* codec, std::string& why);
    bool chooseVideo(const std::string& ext, std::string* error);
    bool chooseAudio(const std::string& ext, std::string* error);
    bool addStream(AVCodecContext* enc, AVStream** out, std::string* error);
    bool setUpScaling(std::string* error);
    bool encode(AVCodecContext* enc, AVStream* st, AVFrame* frame);
    bool drain(AVCodecContext* enc, AVStream* st);
    void quantize(const AVFrame& bgra, AVFrame* out);
    void toLinearFloat(const AVFrame& bgra, AVFrame* out);
    bool queueAudio(const uint8_t** in, int count);
    bool encodeQueuedAudio(bool everything);
    void release();

    EncodeSettings s_;
    std::string description_;
    bool headerWritten_ = false, finished_ = false, failed_ = false;
    std::string firstError_;
    PacketPtr pkt_;

    // frames
    CodecPtr venc_;
    AVStream* vst_ = nullptr;
    int encW_ = 0, encH_ = 0;       // the encoder's frame size
    SwsPtr sws_;
    FramePtr bgra_;                 // the caller's frame, top row first, in a buffer padded the way swscale likes
    // frames given at another size (EncodeSettings::inWidth): scaled into 'fit' inside bgra_ (black round it)
    int inW_ = 0, inH_ = 0;
    bool scaling_ = false, alpha_ = false, interlaced_ = false, topFirst_ = false;
    int fitX_ = 0, fitY_ = 0, fitW_ = 0, fitH_ = 0;
    SwsPtr fitSws_;
    FramePtr src_;                  // the caller's frame
    FramePtr vframe_;
    int64_t videoFrames_ = 0;

    // sound
    CodecPtr aenc_;
    AVStream* ast_ = nullptr;
    SwrPtr swr_;
    FifoPtr fifo_;                  // converted sound waiting to make up a whole encoder frame
    FramePtr aconv_;                // where the resampler writes
    int aconvRoom_ = 0;
    FramePtr aframe_;
    int aFrameSize_ = kFreeAudioFrame;
    bool aPadLast_ = false;         // the encoder insists on whole frames
    int64_t audioSamples_ = 0;
    bool aStartAtZero_ = false;     // AVI: see drain()
    bool aFirstPacket_ = true;
    int64_t aPacketShift_ = 0;

    OutputPtr oc_;                  // the file; release() frees the encoders before it
};

bool FFWriter::fail(const std::string& what, int err)
{
    if (!failed_) {
        failed_ = true;
        firstError_ = err ? what + ": " + errText(err) + "." : what + ".";
        logf("media: writing %s: %s", pathFileName(s_.path).c_str(), firstError_.c_str());
    }
    return false;
}

void FFWriter::release()
{
    // Encoders first (they may still be holding frames), then the file.
    venc_.reset();
    aenc_.reset();
    sws_.reset();
    fitSws_.reset();
    src_.reset();
    swr_.reset();
    fifo_.reset();
    bgra_.reset();
    vframe_.reset();
    aconv_.reset();
    aframe_.reset();
    pkt_.reset();
    vst_ = ast_ = nullptr;
    oc_.reset();
}

// The Master Display of the settings as FFmpeg's mastering display metadata (CIE xy chromaticities, red green blue).
static void fillMasteringDisplay(AVMasteringDisplayMetadata* m, const EncodeSettings& s)
{
    const double* prim[3] = {s.mdRed, s.mdGreen, s.mdBlue};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 2; ++k) m->display_primaries[i][k] = av_d2q(prim[i][k], 50000);
    for (int k = 0; k < 2; ++k) m->white_point[k] = av_d2q(s.mdWhite[k], 50000);
    m->min_luminance = av_d2q(s.mdMinNits, 10000);
    m->max_luminance = av_d2q(s.mdMaxNits, 10000);
    m->has_primaries = 1;
    m->has_luminance = 1;
}

bool FFWriter::openVideoEncoder(const AVCodec* codec, std::string& why)
{
    CodecPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        why = "out of memory";
        return false;
    }
    AVCodecContext* c = ctx.get();
    const std::string name = codec->name;
    const AVPixelFormat pixFmt = choosePixelFormat(codec, s_);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);
    const bool rgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
    const bool jpeg = isJpegRangeFormat(pixFmt);
    int num = s_.fpsNum, den = s_.fpsDen;
    av_reduce(&num, &den, s_.fpsNum, s_.fpsDen, 1000000);
    const double fps = (double)num / den;
    // Formats that store colour at half resolution need even sizes (and so do all the H.264 encoders):
    // the size is rounded down, and the odd column or row of each frame is left off.
    int w = s_.width, h = s_.height;
    if (!desc || rgb == false) {
        if (!desc || desc->log2_chroma_w > 0 || codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC) w &= ~1;
        if (!desc || desc->log2_chroma_h > 0 || codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC) h &= ~1;
    }
    const bool hw = name.find("_nvenc") != std::string::npos || name.find("_qsv") != std::string::npos || name.find("_amf") != std::string::npos;
    const bool x26x = name == "libx264" || name == "libx265";
    const int64_t kbps = std::max(100, s_.videoKbps);
    const bool quality = s_.rateControl == "quality";
    const bool cbr = s_.rateControl == "cbr";

    c->width = w;
    c->height = h;
    c->pix_fmt = pixFmt;
    c->time_base = av_make_q(den, num);
    c->framerate = av_make_q(num, den);
    c->sample_aspect_ratio = av_make_q(1, 1);
    if (s_.pixelAspect > 0 && std::fabs(s_.pixelAspect - 1.0) > 1e-4) c->sample_aspect_ratio = av_d2q(s_.pixelAspect, 10000);
    c->bit_rate = quality ? 0 : kbps * 1000;
    // a key frame every two seconds unless the template says otherwise
    if (s_.keyframeSeconds > 0) c->gop_size = std::max(1, (int)std::lround(fps * s_.keyframeSeconds));
    c->max_b_frames = s_.bFrames >= 0 ? s_.bFrames : (name == "libx264" ? 2 : 0);
    if (!quality) {
        if (cbr) {
            c->rc_max_rate = c->bit_rate;
            if (!hw && !x26x) c->rc_min_rate = c->bit_rate;
            c->rc_buffer_size = (int)std::min<int64_t>(INT32_MAX, c->bit_rate);
        } else if (s_.maxKbps > 0) {
            c->rc_max_rate = std::max<int64_t>(c->bit_rate, (int64_t)s_.maxKbps * 1000);
            c->rc_buffer_size = (int)std::min<int64_t>(INT32_MAX, c->rc_max_rate * 2);
        }
    }
    if (!rgb) {
        // JPEG-range formats are BT.601 by convention; otherwise BT.709 for HD and BT.601 below it.
        const bool hd = isHD(w, h) && !jpeg;
        c->color_range = jpeg ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        c->colorspace = hd ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        c->color_primaries = hd ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
        c->color_trc = hd ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
    }
    // an HDR frame (PQ or HLG coded): the colour tags say so
    if (!rgb && !s_.hdrTransfer.empty()) {
        const bool p3 = s_.hdrPrimaries == "p3d65";
        c->color_trc = s_.hdrTransfer == "hlg" ? AVCOL_TRC_ARIB_STD_B67 : AVCOL_TRC_SMPTE2084;
        c->color_primaries = p3 ? AVCOL_PRI_SMPTE432 : AVCOL_PRI_BT2020;
        c->colorspace = p3 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT2020_NCL;
    }
    // interlaced: say which field comes first, and code the fields as such where the encoder can
    if (s_.fieldOrder == "upper" || s_.fieldOrder == "lower") {
        c->field_order = s_.fieldOrder == "upper" ? AV_FIELD_TT : AV_FIELD_BB;
        if (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_MPEG2VIDEO || codec->id == AV_CODEC_ID_MPEG4)
            c->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
    }
    if (s_.pass == 1) c->flags |= AV_CODEC_FLAG_PASS1;
    if (s_.pass == 2) c->flags |= AV_CODEC_FLAG_PASS2;
    if (oc_->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (codec->capabilities & AV_CODEC_CAP_OTHER_THREADS) c->thread_count = 0;      // x264, libvpx: use every processor
    if (processorLimit() > 0) c->thread_count = processorLimit();
    if (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    // Each encoder's own words for the template's settings.
    Dict options;
    const int q = std::max(0, std::min(51, s_.quality));
    if (name == "libvpx" || name == "libvpx-vp9") {
        options.set("deadline", "good");
        options.set("cpu-used", "4");
        if (name == "libvpx-vp9") options.set("row-mt", "1");
        if (quality) options.set("crf", strfmt("%d", std::min(63, q + 8)).c_str());
        if (hasAlpha(pixFmt)) options.set("auto-alt-ref", "0");
    }
    // AV1, Theora, OpenEXR
    if (name == "libsvtav1") {
        options.set("preset", "8");
        if (quality) options.set("crf", strfmt("%d", std::min(63, q + 12)).c_str());
    } else if (name == "libaom-av1") {
        options.set("cpu-used", "6");
        options.set("row-mt", "1");
        if (quality) options.set("crf", strfmt("%d", std::min(63, q + 12)).c_str());
    } else if (name == "librav1e") {
        options.set("speed", "8");
        if (quality) options.set("qp", strfmt("%d", std::min(255, q * 5)).c_str());
    } else if (name == "libtheora" && quality) {
        c->flags |= AV_CODEC_FLAG_QSCALE;           // Theora's quality: 0 .. 10 (best)
        c->global_quality = FF_QP2LAMBDA * std::max(0, std::min(10, 10 - q / 5));
    } else if (codec->id == AV_CODEC_ID_EXR) {
        options.set("compression", "zip16");
        options.set("format", "half");
    }
    if (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC) {
        if (!s_.profile.empty()) options.set("profile", s_.profile.c_str());
        if (!s_.level.empty()) options.set("level", s_.level.c_str());
    }
    if (name == "libx264") {
        if (quality) options.set("crf", strfmt("%d", q).c_str());
        if (cbr && !quality) options.set("nal-hrd", "cbr");
        if (s_.pass && !s_.passLog.empty()) options.set("stats", s_.passLog.c_str());
    } else if (name == "libx265") {
        std::string params = "log-level=error";
        if (quality) params += strfmt(":crf=%d", q);
        if (s_.pass) params += strfmt(":pass=%d", s_.pass) + (s_.passLog.empty() ? std::string() : ":stats=" + s_.passLog);
        options.set("x265-params", params.c_str());
    } else if (name.find("_nvenc") != std::string::npos) {
        options.set("rc", quality ? "vbr" : cbr ? "cbr" : "vbr");
        if (quality) options.set("cq", strfmt("%d", q).c_str());
        if (s_.pass) options.set("multipass", "fullres");     // NVENC does both passes itself, in one
    } else if (name.find("_qsv") != std::string::npos) {
        if (quality) options.set("global_quality", strfmt("%d", q).c_str());
    } else if (name.find("_amf") != std::string::npos) {
        options.set("rc", quality ? "cqp" : cbr ? "cbr" : "vbr_peak");
        if (quality) {
            options.set("qp_i", strfmt("%d", q).c_str());
            options.set("qp_p", strfmt("%d", q).c_str());
        }
    } else if (isMfEncoder(codec)) {
        options.set("rate_control", quality ? "quality" : cbr ? "cbr" : "pc_vbr");
        if (quality) options.set("quality", strfmt("%d", std::max(1, 100 - q * 2)).c_str());
    } else if (codec->id == AV_CODEC_ID_PRORES && name == "prores_ks") {
        options.set("profile", s_.profile.empty() ? "standard" : s_.profile.c_str());
        options.set("vendor", "apl0");
    } else if (codec->id == AV_CODEC_ID_DNXHD) {
        options.set("profile", s_.profile.empty() ? "dnxhr_hq" : s_.profile.c_str());
    } else if (quality && (codec->id == AV_CODEC_ID_MPEG4 || codec->id == AV_CODEC_ID_MPEG2VIDEO || codec->id == AV_CODEC_ID_MJPEG ||
                           codec->id == AV_CODEC_ID_WMV2 || codec->id == AV_CODEC_ID_WMV1)) {
        // the older encoders' constant quality: a fixed quantiser (2 best .. 31)
        c->flags |= AV_CODEC_FLAG_QSCALE;
        c->global_quality = FF_QP2LAMBDA * std::max(2, std::min(31, 2 + q * 29 / 51));
    }
    // the template's own encoder options (FFmpeg's words, "key=value" separated by spaces; the x264/x265 parameter strings are added to)
    {
        std::istringstream in(s_.encoderOptions);
        std::string token;
        while (in >> token) {
            const size_t eq = token.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            const std::string key = token.substr(0, eq), value = token.substr(eq + 1);
            std::string text = value;
            if ((key == "x265-params" && name == "libx265") || (key == "x264-params" && name == "libx264")) {
                const AVDictionaryEntry* have = av_dict_get(options.d, key.c_str(), nullptr, 0);
                if (have && have->value && *have->value) text = std::string(have->value) + ":" + value;
            }
            options.set(key.c_str(), text.c_str());
        }
    }
    const int r = avcodec_open2(c, codec, &options.d);
    if (r < 0) {
        why = errText(r);
        return false;
    }
    venc_ = std::move(ctx);
    encW_ = w;
    encH_ = h;
    return true;
}

bool FFWriter::openAudioEncoder(const AVCodec* codec, std::string& why)
{
    CodecPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        why = "out of memory";
        return false;
    }
    AVCodecContext* c = ctx.get();
    const std::string name = codec->name;
    c->sample_fmt = chooseSampleFormat(codec);
    c->sample_rate = chooseSampleRate(codec, s_.outSampleRate > 0 ? s_.outSampleRate : s_.sampleRate);
    if (!chooseChannelLayout(codec, &c->ch_layout, s_.audioChannels == 1 ? 1 : (s_.audioChannels >= 3 && s_.audioChannels <= 32 ? s_.audioChannels : 2))) {
        why = "out of memory";
        return false;
    }
    c->time_base = av_make_q(1, c->sample_rate);
    int64_t bitRate = (int64_t)std::max(16, s_.audioKbps) * 1000;
    if (name == "aac_mf") {
        // The Media Foundation AAC encoder only does these rates.
        const int64_t allowed[] = {96000, 128000, 160000, 192000};
        int64_t best = allowed[0];
        for (int64_t a : allowed)
            if (std::llabs(a - bitRate) < std::llabs(best - bitRate)) best = a;
        bitRate = best;
    }
    c->bit_rate = bitRate;
    if (oc_->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    const int r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
        why = errText(r);
        return false;
    }
    aenc_ = std::move(ctx);
    return true;
}

bool FFWriter::chooseVideo(const std::string& ext, std::string* error)
{
    std::vector<Candidate> list;
    if (!s_.videoCodec.empty()) {
        Candidate c;
        c.name = s_.videoCodec;
        c.codec = avcodec_find_encoder_by_name(s_.videoCodec.c_str());
        if (c.codec && (!av_codec_is_encoder(c.codec) || c.codec->type != AVMEDIA_TYPE_VIDEO)) c.codec = nullptr;
        list.push_back(c);
    } else {
        list = videoCandidates(ext, oc_->oformat, s_.path, s_);
    }
    std::string tried;
    for (const Candidate& c : list) {
        std::string why = "not in this FFmpeg";
        if (c.codec && c.codec->type == AVMEDIA_TYPE_VIDEO && openVideoEncoder(c.codec, why)) return true;
        if (!tried.empty()) tried += "; ";
        tried += c.name + ": " + why;
    }
    if (!s_.videoCodec.empty()) setError(error, "The video encoder " + s_.videoCodec + " cannot be used (" + tried + ").");
    else if (list.empty()) setError(error, "This FFmpeg has no video encoder for ." + ext + " files.");
    else setError(error, "No video encoder for ." + ext + " files could be started (" + tried + ").");
    return false;
}

bool FFWriter::chooseAudio(const std::string& ext, std::string* error)
{
    std::vector<Candidate> list;
    if (!s_.audioCodec.empty()) {
        Candidate c;
        c.name = s_.audioCodec;
        c.codec = avcodec_find_encoder_by_name(s_.audioCodec.c_str());
        if (c.codec && (!av_codec_is_encoder(c.codec) || c.codec->type != AVMEDIA_TYPE_AUDIO)) c.codec = nullptr;
        list.push_back(c);
    } else {
        list = audioCandidates(ext, oc_->oformat, s_.path, s_);
    }
    std::string tried;
    for (const Candidate& c : list) {
        std::string why = "not in this FFmpeg";
        if (c.codec && c.codec->type == AVMEDIA_TYPE_AUDIO && openAudioEncoder(c.codec, why)) return true;
        if (!tried.empty()) tried += "; ";
        tried += c.name + ": " + why;
    }
    if (!s_.audioCodec.empty()) setError(error, "The audio encoder " + s_.audioCodec + " cannot be used (" + tried + ").");
    else if (list.empty()) setError(error, "This FFmpeg has no audio encoder for ." + ext + " files.");
    else setError(error, "No audio encoder for ." + ext + " files could be started (" + tried + ").");
    return false;
}

bool FFWriter::addStream(AVCodecContext* enc, AVStream** out, std::string* error)
{
    AVStream* st = avformat_new_stream(oc_.get(), nullptr);
    if (!st) {
        setError(error, "Out of memory.");
        return false;
    }
    st->time_base = enc->time_base;         // a suggestion
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
        st->avg_frame_rate = enc->framerate;
        st->sample_aspect_ratio = enc->sample_aspect_ratio;
    }
    const int r = avcodec_parameters_from_context(st->codecpar, enc);
    if (r < 0) {
        setError(error, "Cannot describe the " + std::string(enc->codec->name) + " stream to the container: " + errText(r) + ".");
        return false;
    }
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO && s_.spherical) {
        // a 360° video: equirectangular spherical side data
        size_t size = 0;
        AVSphericalMapping* sph = av_spherical_alloc(&size);
        if (sph) {
            sph->projection = AV_SPHERICAL_EQUIRECTANGULAR;
            if (!av_packet_side_data_add(&st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data, AV_PKT_DATA_SPHERICAL, sph, size, 0))
                av_free(sph);
            oc_->strict_std_compliance = std::min(oc_->strict_std_compliance, FF_COMPLIANCE_UNOFFICIAL);
        }
    }
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO && s_.mastering) {
        // the Master Display (Project Properties): mastering display colour volume and content light level for the container
        size_t size = sizeof(AVMasteringDisplayMetadata);
        AVMasteringDisplayMetadata* md = av_mastering_display_metadata_alloc();
        if (md) {
            fillMasteringDisplay(md, s_);
            if (!av_packet_side_data_add(&st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, md, size, 0))
                av_free(md);
        }
        if (s_.maxCll > 0 || s_.maxFall > 0) {
            AVContentLightMetadata* cl = av_content_light_metadata_alloc(&size);
            if (cl) {
                cl->MaxCLL = (unsigned)s_.maxCll;
                cl->MaxFALL = (unsigned)s_.maxFall;
                if (!av_packet_side_data_add(&st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, cl, size, 0))
                    av_free(cl);
            }
        }
    }
    *out = st;
    return true;
}

bool FFWriter::open(const EncodeSettings& settings, std::string* error)
{
    s_ = settings;
    const std::string file = pathFileName(s_.path);
    const std::string ext = pathLowerExtension(s_.path);

    AVFormatContext* oc = nullptr;
    int r = avformat_alloc_output_context2(&oc, nullptr, nullptr, s_.path.c_str());
    if (r < 0 || !oc) {
        setError(error, "Cannot write a ." + ext + " file (" + file + "): " + errText(r) + ".");
        return false;
    }
    oc_.reset(oc);
    const AVOutputFormat* format = oc->oformat;

    // Containers that hold only sound or only frames simply do not get the other.
    static const char* const audioOnly[] = {"wav", "w64", "mp3", "flac", "m4a", "aac", "ogg", "oga", "opus", "wma", "ac3", "aiff", "aif", "aifc",
                                            "mka", "mp2", "caf", "au", "eac3", "wv", "tta", nullptr};
    const bool wantVideo = s_.video && format->video_codec != AV_CODEC_ID_NONE && !oneOf(ext, audioOnly);
    const bool wantAudio = s_.audio && format->audio_codec != AV_CODEC_ID_NONE;
    if (!wantVideo && !wantAudio) {
        setError(error, s_.video || s_.audio ? "A ." + ext + " file cannot hold this video or audio." : "There is no video or audio to write.");
        return false;
    }
    if (wantVideo) {
        if (s_.width < 2 || s_.height < 2 || s_.width > 32768 || s_.height > 32768 || s_.fpsNum < 1 || s_.fpsDen < 1) {
            setError(error, strfmt("Cannot write %d x %d video at %d/%d fps.", s_.width, s_.height, s_.fpsNum, s_.fpsDen));
            return false;
        }
        if (!chooseVideo(ext, error) || !addStream(venc_.get(), &vst_, error)) return false;

        // The frame converter: the caller's BGRA to what the encoder takes.
        const AVPixelFormat pixFmt = venc_->pix_fmt;
        if (!madeHere(pixFmt))
            sws_.reset(sws_getContext(encW_, encH_, AV_PIX_FMT_BGRA, encW_, encH_, pixFmt, SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr));
        if (!sws_ && !madeHere(pixFmt)) {
            setError(error, strfmt("Cannot convert frames to %s for the %s encoder.", av_get_pix_fmt_name(pixFmt), venc_->codec->name));
            return false;
        }
        const int space = venc_->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : venc_->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601;
        const int* coefficients = sws_getCoefficients(space);
        // The source is RGB (its table and range are not used); the destination's matrix and range are the encoder's.
        if (sws_) sws_setColorspaceDetails(sws_.get(), coefficients, 0, coefficients, venc_->color_range == AVCOL_RANGE_JPEG ? 1 : 0, 0, 1 << 16, 1 << 16);

        bgra_.reset(av_frame_alloc());
        vframe_.reset(av_frame_alloc());
        if (!bgra_ || !vframe_) {
            setError(error, "Out of memory.");
            return false;
        }
        bgra_->format = AV_PIX_FMT_BGRA;
        bgra_->width = encW_;
        bgra_->height = encH_;
        r = av_frame_get_buffer(bgra_.get(), 0);
        if (r < 0) {
            setError(error, "Out of memory for a " + strfmt("%d x %d", encW_, encH_) + " picture: " + errText(r) + ".");
            return false;
        }
        alpha_ = s_.alpha && (hasAlpha(pixFmt) || pixFmt == AV_PIX_FMT_PAL8);
        interlaced_ = s_.fieldOrder == "upper" || s_.fieldOrder == "lower";
        topFirst_ = s_.fieldOrder == "upper";
        if (!setUpScaling(error)) return false;
    }
    if (wantAudio) {
        if (s_.sampleRate < 1000 || s_.sampleRate > 768000) {
            setError(error, strfmt("Cannot write audio at %d Hz.", s_.sampleRate));
            return false;
        }
        if (!chooseAudio(ext, error) || !addStream(aenc_.get(), &ast_, error)) return false;

        aFrameSize_ = aenc_->frame_size > 0 ? aenc_->frame_size : kFreeAudioFrame;
        aPadLast_ = aenc_->frame_size > 0 && !(aenc_->codec->capabilities & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE));

        // Converts the caller's stereo or 5.1 (L R C LFE Ls Rs) float to what the encoder takes.
        Layout in;
        inChannels_ = s_.audioChannels >= 3 && s_.audioChannels <= 32 ? s_.audioChannels : 2;
        if (inChannels_ > 2 && aenc_->ch_layout.nb_channels == inChannels_) r = av_channel_layout_copy(&in.l, &aenc_->ch_layout);
        else av_channel_layout_default(&in.l, inChannels_);
        SwrContext* swr = nullptr;
        r = swr_alloc_set_opts2(&swr, &aenc_->ch_layout, aenc_->sample_fmt, aenc_->sample_rate, &in.l, AV_SAMPLE_FMT_FLTP, s_.sampleRate, 0, nullptr);
        if (r >= 0 && swr) {
            swr_.reset(swr);
            r = swr_init(swr);
        }
        if (r < 0 || !swr_) {
            setError(error, strfmt("Cannot convert audio for the %s encoder: %s.", aenc_->codec->name, errText(r).c_str()));
            return false;
        }
        fifo_.reset(av_audio_fifo_alloc(aenc_->sample_fmt, aenc_->ch_layout.nb_channels, aFrameSize_ * 4));
        aconv_.reset(av_frame_alloc());
        aframe_.reset(av_frame_alloc());
        if (!fifo_ || !aconv_ || !aframe_) {
            setError(error, "Out of memory.");
            return false;
        }
    }
    pkt_.reset(av_packet_alloc());
    if (!pkt_) {
        setError(error, "Out of memory.");
        return false;
    }
    aStartAtZero_ = format->name && std::strcmp(format->name, "avi") == 0;

    if (!(format->flags & AVFMT_NOFILE)) {
        r = avio_open(&oc->pb, s_.path.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) {
            setError(error, "Cannot create " + file + ": " + errText(r) + ".");
            return false;
        }
    }
    // chapters (their ends are filled in when the length is known, in finish()) and text tags.
    static const char* const quickTimeChapters[] = {"mp4", "m4v", "m4a", "mov", "3gp", nullptr};
    std::vector<std::pair<double, std::string>> chapters = s_.chapters;
    if (!chapters.empty() && chapters.front().first > 0.0005 && oneOf(ext, quickTimeChapters))
        chapters.insert(chapters.begin(), std::make_pair(0.0, std::string("Start")));
    for (size_t k = 0; k < chapters.size(); ++k) {
        AVChapter* ch = (AVChapter*)av_mallocz(sizeof(AVChapter));
        if (!ch) break;
        ch->id = (int64_t)k + 1;
        ch->time_base = av_make_q(1, 1000);
        ch->start = (int64_t)std::llround(std::max(0.0, chapters[k].first) * 1000.0);
        ch->end = ch->start + 1;
        if (!chapters[k].second.empty()) av_dict_set(&ch->metadata, "title", chapters[k].second.c_str(), 0);
        av_dynarray_add(&oc->chapters, (int*)&oc->nb_chapters, ch);
        if (!oc->chapters) {
            oc->nb_chapters = 0;
            break;
        }
    }
    for (size_t k = 1; k < oc->nb_chapters; ++k) oc->chapters[k - 1]->end = std::max(oc->chapters[k - 1]->start + 1, oc->chapters[k]->start);
    for (const auto& m : s_.metadata)
        if (!m.first.empty() && !m.second.empty()) av_dict_set(&oc->metadata, m.first.c_str(), m.second.c_str(), 0);

    Dict options;
    static const char* const movFamily[] = {"mp4", "m4v", "m4a", "mov", "3gp", "3g2", nullptr};
    if (oneOf(ext, movFamily)) options.set("movflags", "+faststart");     // the index goes at the front
    if (format->name && std::strcmp(format->name, "image2") == 0) options.set("start_number", "0");
    r = avformat_write_header(oc, &options.d);
    if (r < 0) {
        setError(error, "Cannot start writing " + file + ": " + errText(r) + ".");
        return false;
    }
    headerWritten_ = true;

    // What was chosen, for the log.
    description_ = std::string(format->name) + ":";
    if (venc_) {
        const double fps = av_q2d(venc_->framerate);
        const std::string rate = std::fabs(fps - std::round(fps)) < 0.0005 ? strfmt("%d", (int)std::lround(fps)) : strfmt("%.2f", fps);
        description_ += strfmt(" %s %dx%d %s fps", venc_->codec->name, encW_, encH_, rate.c_str());
        const AVCodecDescriptor* d = avcodec_descriptor_get(venc_->codec_id);
        if (d && (d->props & AV_CODEC_PROP_LOSSY) && !(d->props & AV_CODEC_PROP_LOSSLESS) && venc_->bit_rate > 0)
            description_ += strfmt(" %d kb/s", (int)(venc_->bit_rate / 1000));
    }
    if (aenc_) {
        if (venc_) description_ += ",";
        description_ += strfmt(" %s %d Hz", aenc_->codec->name, aenc_->sample_rate);
        const AVCodecDescriptor* d = avcodec_descriptor_get(aenc_->codec_id);
        if (d && (d->props & AV_CODEC_PROP_LOSSY) && !(d->props & AV_CODEC_PROP_LOSSLESS) && aenc_->bit_rate > 0)
            description_ += strfmt(" %d kb/s", (int)(aenc_->bit_rate / 1000));
    }
    return true;
}

// Hands every packet the encoder has ready to the container.
bool FFWriter::drain(AVCodecContext* enc, AVStream* st)
{
    AVPacket* pkt = pkt_.get();
    for (;;) {
        int r = avcodec_receive_packet(enc, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;      // it wants more input / it has given everything
        if (r < 0) return fail(std::string("The ") + enc->codec->name + " encoder failed", r);
        if (enc->codec_type == AVMEDIA_TYPE_VIDEO && pkt->duration <= 0) pkt->duration = 1;     // one frame, in the encoder's units
        if (enc->codec_type == AVMEDIA_TYPE_AUDIO && aStartAtZero_) {
            // An AAC or MP3 encoder's first packet is stamped before zero (its lead-in).
            if (aFirstPacket_) {
                aFirstPacket_ = false;
                const int64_t first = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
                if (first != AV_NOPTS_VALUE && first < 0) aPacketShift_ = -first;
            }
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += aPacketShift_;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += aPacketShift_;
        }
        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        pkt->stream_index = st->index;
        r = av_interleaved_write_frame(oc_.get(), pkt);                 // takes the packet's contents whatever happens
        av_packet_unref(pkt);
        if (r < 0) return fail("Writing to " + pathFileName(s_.path) + " failed", r);
    }
}

// Gives the encoder a frame (null: no more frames are coming) and writes out what it produces.
bool FFWriter::encode(AVCodecContext* enc, AVStream* st, AVFrame* frame)
{
    for (int tries = 0;; ++tries) {
        const int r = avcodec_send_frame(enc, frame);
        if (r == AVERROR(EAGAIN) && tries < 100) {      // it must be emptied before it takes more
            if (!drain(enc, st)) return false;
            continue;
        }
        if (r == AVERROR_EOF && !frame) break;          // it had already been told
        if (r < 0) return fail(std::string("The ") + enc->codec->name + " encoder refused a frame", r);
        break;
    }
    return drain(enc, st);
}

bool FFWriter::setUpScaling(std::string* error)
{
    inW_ = s_.inWidth > 0 ? s_.inWidth : s_.width;
    inH_ = s_.inHeight > 0 ? s_.inHeight : s_.height;
    scaling_ = inW_ != s_.width || inH_ != s_.height ||
               (!s_.stretch && s_.inWidth > 0 && std::fabs(s_.inPixelAspect - s_.pixelAspect) > 1e-4);
    // black (or, keeping transparency, clear) round the frame
    for (int y = 0; y < encH_; ++y) {
        uint8_t* row = bgra_->data[0] + (ptrdiff_t)y * bgra_->linesize[0];
        std::memset(row, 0, (size_t)encW_ * 4);
        if (!alpha_)
            for (int x = 0; x < encW_; ++x) row[x * 4 + 3] = 255;
    }
    if (!scaling_) return true;
    if (inW_ < 1 || inH_ < 1 || inW_ > 32768 || inH_ > 32768) {
        setError(error, strfmt("Unsupported picture size %d x %d.", inW_, inH_));
        return false;
    }
    // where the frame goes: the whole frame (stretched), or as large as fits keeping its shape (letterboxed)
    fitX_ = 0;
    fitY_ = 0;
    fitW_ = encW_;
    fitH_ = encH_;
    if (!s_.stretch) {
        const double inPar = s_.inPixelAspect > 0 ? s_.inPixelAspect : 1.0, outPar = s_.pixelAspect > 0 ? s_.pixelAspect : 1.0;
        const double shape = inW_ * inPar / inH_;               // the frame's shape as seen
        const double frame = encW_ * outPar / encH_;             // the file's
        if (shape > frame + 1e-6) fitH_ = std::max(2, (int)std::lround(encW_ * outPar / shape)) & ~1;
        else if (shape < frame - 1e-6) fitW_ = std::max(2, (int)std::lround(encH_ * shape / outPar)) & ~1;
        fitW_ = std::min(fitW_, encW_);
        fitH_ = std::min(fitH_, encH_);
        fitX_ = ((encW_ - fitW_) / 2) & ~1;
        fitY_ = ((encH_ - fitH_) / 2) & ~1;
    }
    fitSws_.reset(sws_getContext(inW_, inH_, AV_PIX_FMT_BGRA, fitW_, fitH_, AV_PIX_FMT_BGRA, SWS_BICUBIC, nullptr, nullptr, nullptr));
    src_.reset(av_frame_alloc());
    if (!fitSws_ || !src_) {
        setError(error, "Out of memory.");
        return false;
    }
    src_->format = AV_PIX_FMT_BGRA;
    src_->width = inW_;
    src_->height = inH_;
    const int r = av_frame_get_buffer(src_.get(), 0);
    if (r < 0) {
        setError(error, "Out of memory for a " + strfmt("%d x %d", inW_, inH_) + " picture: " + errText(r) + ".");
        return false;
    }
    return true;
}

bool FFWriter::writeVideo(const Image& frame)
{
    if (finished_ || failed_) return false;
    if (!venc_) return true;                // this file has no frames: nothing to do
    if (frame.depth != PixelDepth::Byte || frame.w != inW_ || frame.h != inH_ || !frame.data || frame.rowBytes < (size_t)frame.w * 4)
        return fail(strfmt("Frame size %d x %d does not match the output size %d x %d.", frame.w, frame.h, inW_, inH_));

    // Copy the frame top row first. Image row 0 is the BOTTOM of the frame, so the top row is row h-1.
    int r = av_frame_make_writable(bgra_.get());
    if (r < 0) return fail("Out of memory", r);
    if (scaling_) {
        r = av_frame_make_writable(src_.get());
        if (r < 0) return fail("Out of memory", r);
        for (int y = 0; y < inH_; ++y) std::memcpy(src_->data[0] + (ptrdiff_t)y * src_->linesize[0], frame.row(inH_ - 1 - y), (size_t)inW_ * 4);
        uint8_t* dst[4] = {bgra_->data[0] + (ptrdiff_t)fitY_ * bgra_->linesize[0] + (ptrdiff_t)fitX_ * 4, nullptr, nullptr, nullptr};
        int dstStride[4] = {bgra_->linesize[0], 0, 0, 0};
        if (sws_scale(fitSws_.get(), src_->data, src_->linesize, 0, inH_, dst, dstStride) <= 0) return fail("Could not scale the frame.");
    } else {
        for (int y = 0; y < encH_; ++y)
            std::memcpy(bgra_->data[0] + (ptrdiff_t)y * bgra_->linesize[0], frame.row(s_.height - 1 - y), (size_t)encW_ * 4);
    }
    if (alpha_) {
        // the frames are premultiplied; files keep colour and transparency apart
        for (int y = 0; y < encH_; ++y) {
            uint8_t* p = bgra_->data[0] + (ptrdiff_t)y * bgra_->linesize[0];
            for (int x = 0; x < encW_; ++x, p += 4) {
                const int a = p[3];
                if (a == 0 || a == 255) continue;
                for (int c = 0; c < 3; ++c) p[c] = (uint8_t)std::min(255, (p[c] * 255 + a / 2) / a);
            }
        }
    }

    // A new buffer for every frame: the encoder may keep hold of the last one for a while.
    AVFrame* f = vframe_.get();
    av_frame_unref(f);
    f->format = venc_->pix_fmt;
    f->width = encW_;
    f->height = encH_;
    r = av_frame_get_buffer(f, 0);
    if (r < 0) return fail("Out of memory", r);
    if (f->format == AV_PIX_FMT_PAL8) quantize(*bgra_, f);
    else if (f->format == AV_PIX_FMT_GBRPF32LE || f->format == AV_PIX_FMT_GBRAPF32LE) toLinearFloat(*bgra_, f);
    else if (sws_scale(sws_.get(), bgra_->data, bgra_->linesize, 0, encH_, f->data, f->linesize) <= 0) return fail("Could not convert the frame for the encoder.");
    f->pts = videoFrames_++;                // in the encoder's time base, which is one frame
    f->duration = 1;
    f->color_range = venc_->color_range;
    f->colorspace = venc_->colorspace;
    f->color_primaries = venc_->color_primaries;
    f->color_trc = venc_->color_trc;
    f->sample_aspect_ratio = venc_->sample_aspect_ratio;
    if (s_.mastering) {
        if (AVMasteringDisplayMetadata* md = av_mastering_display_metadata_create_side_data(f)) fillMasteringDisplay(md, s_);
        if (s_.maxCll > 0 || s_.maxFall > 0)
            if (AVContentLightMetadata* cl = av_content_light_metadata_create_side_data(f)) {
                cl->MaxCLL = (unsigned)s_.maxCll;
                cl->MaxFALL = (unsigned)s_.maxFall;
            }
    }
    if (interlaced_) f->flags |= AV_FRAME_FLAG_INTERLACED | (topFirst_ ? AV_FRAME_FLAG_TOP_FIELD_FIRST : 0);
    const bool ok = encode(venc_.get(), vst_, f);
    av_frame_unref(f);
    return ok;
}

// GIF's frames: at most 256 colours each, chosen for each frame.
void FFWriter::quantize(const AVFrame& bgra, AVFrame* out)
{
    const int w = encW_, h = encH_;
    struct Cell { uint32_t count = 0; uint64_t b = 0, g = 0, r = 0; };
    std::vector<Cell> cells(32768);
    auto key = [](const uint8_t* p) { return ((p[2] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[0] >> 3); };
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = bgra.data[0] + (ptrdiff_t)y * bgra.linesize[0];
        for (int x = 0; x < w; ++x, p += 4) {
            if (alpha_ && p[3] < 128) continue;
            Cell& c = cells[(size_t)key(p)];
            ++c.count;
            c.b += p[0];
            c.g += p[1];
            c.r += p[2];
        }
    }
    std::vector<int> used;
    for (int i = 0; i < 32768; ++i)
        if (cells[(size_t)i].count) used.push_back(i);
    const int most = alpha_ ? 255 : 256;
    // boxes of cells, split at the median of their widest channel until there are enough
    struct Box { int from, to; };          // a range of 'used'
    std::vector<Box> boxes;
    if (!used.empty()) boxes.push_back({0, (int)used.size()});
    auto channel = [](int cell, int ch) { return ch == 0 ? (cell >> 10) & 31 : ch == 1 ? (cell >> 5) & 31 : cell & 31; };
    while ((int)boxes.size() < most) {
        int pick = -1, pickRange = 0, pickCh = 0;
        uint64_t pickWeight = 0;
        for (int k = 0; k < (int)boxes.size(); ++k) {
            const Box& b = boxes[(size_t)k];
            if (b.to - b.from < 2) continue;
            int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
            uint64_t weight = 0;
            for (int i = b.from; i < b.to; ++i) {
                for (int ch = 0; ch < 3; ++ch) {
                    lo[ch] = std::min(lo[ch], channel(used[(size_t)i], ch));
                    hi[ch] = std::max(hi[ch], channel(used[(size_t)i], ch));
                }
                weight += cells[(size_t)used[(size_t)i]].count;
            }
            int ch = 0;
            for (int c2 = 1; c2 < 3; ++c2)
                if (hi[c2] - lo[c2] > hi[ch] - lo[ch]) ch = c2;
            const int range = hi[ch] - lo[ch];
            // the box with the most pixels spread the widest
            if (range > 0 && (pick < 0 || (uint64_t)range * weight > (uint64_t)pickRange * pickWeight)) {
                pick = k;
                pickRange = range;
                pickWeight = weight;
                pickCh = ch;
            }
        }
        if (pick < 0) break;
        Box b = boxes[(size_t)pick];
        std::sort(used.begin() + b.from, used.begin() + b.to, [&](int a, int c) { return channel(a, pickCh) < channel(c, pickCh); });
        uint64_t total = 0, half = 0;
        for (int i = b.from; i < b.to; ++i) total += cells[(size_t)used[(size_t)i]].count;
        int cut = b.from + 1;
        for (int i = b.from; i < b.to - 1; ++i) {
            half += cells[(size_t)used[(size_t)i]].count;
            cut = i + 1;
            if (half * 2 >= total) break;
        }
        // never between two cells of the same value on that channel... unless the box is all one value there.
        boxes[(size_t)pick] = {b.from, cut};
        boxes.push_back({cut, b.to});
    }
    uint32_t* pal = (uint32_t*)out->data[1];
    std::vector<int> colour(boxes.size() * 3);
    for (size_t k = 0; k < 256; ++k) pal[k] = 0xff000000u;
    for (size_t k = 0; k < boxes.size(); ++k) {
        uint64_t n = 0, b = 0, g = 0, r = 0;
        for (int i = boxes[k].from; i < boxes[k].to; ++i) {
            const Cell& c = cells[(size_t)used[(size_t)i]];
            n += c.count;
            b += c.b;
            g += c.g;
            r += c.r;
        }
        const int bb = n ? (int)((b + n / 2) / n) : 0, gg = n ? (int)((g + n / 2) / n) : 0, rr = n ? (int)((r + n / 2) / n) : 0;
        colour[k * 3] = bb;
        colour[k * 3 + 1] = gg;
        colour[k * 3 + 2] = rr;
        pal[k] = 0xff000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
    }
    if (alpha_) pal[255] = 0;      // clear
    // each cell's nearest colour
    std::vector<uint8_t> nearest(32768, 0);
    for (int cell : used) {
        const int r = (channel(cell, 0) << 3) | 4, g = (channel(cell, 1) << 3) | 4, b = (channel(cell, 2) << 3) | 4;
        int best = 0, bestD = INT32_MAX;
        for (size_t k = 0; k < boxes.size(); ++k) {
            const int db = colour[k * 3] - b, dg = colour[k * 3 + 1] - g, dr = colour[k * 3 + 2] - r;
            const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (d < bestD) {
                bestD = d;
                best = (int)k;
            }
        }
        nearest[(size_t)cell] = (uint8_t)best;
    }
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = bgra.data[0] + (ptrdiff_t)y * bgra.linesize[0];
        uint8_t* d = out->data[0] + (ptrdiff_t)y * out->linesize[0];
        for (int x = 0; x < w; ++x, p += 4) d[x] = alpha_ && p[3] < 128 ? 255 : nearest[(size_t)key(p)];
    }
}

// OpenEXR's frames: linear light (the sRGB curve taken off), colour multiplied by alpha as EXR keeps it, planes G B R
// (A).
void FFWriter::toLinearFloat(const AVFrame& bgra, AVFrame* out)
{
    static const std::vector<float> table = [] {
        std::vector<float> t(256);
        for (int i = 0; i < 256; ++i) {
            const double v = i / 255.0;
            t[(size_t)i] = (float)(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    const bool a = out->format == AV_PIX_FMT_GBRAPF32LE;
    for (int y = 0; y < encH_; ++y) {
        const uint8_t* p = bgra.data[0] + (ptrdiff_t)y * bgra.linesize[0];
        float* g = (float*)(out->data[0] + (ptrdiff_t)y * out->linesize[0]);
        float* b = (float*)(out->data[1] + (ptrdiff_t)y * out->linesize[1]);
        float* r = (float*)(out->data[2] + (ptrdiff_t)y * out->linesize[2]);
        float* al = a ? (float*)(out->data[3] + (ptrdiff_t)y * out->linesize[3]) : nullptr;
        for (int x = 0; x < encW_; ++x, p += 4) {
            const float m = a ? p[3] / 255.f : 1.f;
            b[x] = table[p[0]] * m;
            g[x] = table[p[1]] * m;
            r[x] = table[p[2]] * m;
            if (al) al[x] = m;
        }
    }
}

// Converts 'count' samples of the caller's sound ('in' null: what the converter is still holding)
// and adds the result to the queue for the encoder.
bool FFWriter::queueAudio(const uint8_t** in, int count)
{
    int room = in ? swr_get_out_samples(swr_.get(), count) : (int)swr_get_delay(swr_.get(), aenc_->sample_rate) + 64;
    if (room < 0) return fail("Could not convert the audio for the encoder.", room);
    room += 64;
    AVFrame* c = aconv_.get();
    if (aconvRoom_ < room) {
        av_frame_unref(c);
        c->format = aenc_->sample_fmt;
        c->sample_rate = aenc_->sample_rate;
        c->nb_samples = room;
        int r = av_channel_layout_copy(&c->ch_layout, &aenc_->ch_layout);
        if (r >= 0) r = av_frame_get_buffer(c, 0);
        if (r < 0) {
            aconvRoom_ = 0;
            return fail("Out of memory", r);
        }
        aconvRoom_ = room;
    }
    const int got = swr_convert(swr_.get(), c->extended_data, aconvRoom_, in, in ? count : 0);
    if (got < 0) return fail("Could not convert the audio for the encoder.", got);
    if (got > 0 && av_audio_fifo_write(fifo_.get(), (void**)c->extended_data, got) < got) return fail("Out of memory");
    return true;
}

// Encodes the queued sound a frame at a time. Unless 'everything', what does not fill a frame stays queued.
bool FFWriter::encodeQueuedAudio(bool everything)
{
    for (;;) {
        const int queued = av_audio_fifo_size(fifo_.get());
        if (queued < 1 || (queued < aFrameSize_ && !everything)) return true;
        const int take = std::min(queued, aFrameSize_);
        const int size = take < aFrameSize_ && aPadLast_ ? aFrameSize_ : take;

        AVFrame* f = aframe_.get();
        av_frame_unref(f);
        f->format = aenc_->sample_fmt;
        f->sample_rate = aenc_->sample_rate;
        f->nb_samples = size;
        int r = av_channel_layout_copy(&f->ch_layout, &aenc_->ch_layout);
        if (r >= 0) r = av_frame_get_buffer(f, 0);
        if (r < 0) return fail("Out of memory", r);
        if (av_audio_fifo_read(fifo_.get(), (void**)f->extended_data, take) < take) return fail("Not enough audio data for the encoder");
        if (size > take) av_samples_set_silence(f->extended_data, take, size - take, f->ch_layout.nb_channels, aenc_->sample_fmt);
        f->pts = audioSamples_;             // in the encoder's time base, which is one sample
        f->duration = size;
        audioSamples_ += size;
        const bool ok = encode(aenc_.get(), ast_, f);
        av_frame_unref(f);
        if (!ok) return false;
    }
}

bool FFWriter::writeAudio(const float* left, const float* right, int frames)
{
    if (finished_ || failed_) return false;
    if (!aenc_ || frames <= 0) return true; // this file has no sound, or there is nothing to add
    if (!left) left = right;                // one side missing: use the other for both
    if (!right) right = left;
    static const float silence[kAudioChunk] = {0};
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, kAudioChunk);
        const uint8_t* in[32];
        in[0] = (const uint8_t*)(left ? left + done : silence);
        in[1] = (const uint8_t*)(right ? right + done : silence);
        for (int c = 2; c < 32; ++c) in[c] = (const uint8_t*)silence;
        if (!queueAudio(in, n) || !encodeQueuedAudio(false)) return false;
        done += n;
    }
    return true;
}

bool FFWriter::writeAudioChannels(const float* const* ch, int channels, int frames)
{
    if (channels != inChannels_ || inChannels_ <= 2) {
        if (channels > 2 && inChannels_ > 2) {
            static const float none[kAudioChunk] = {0};
            for (int done = 0; done < frames;) {
                const int n = std::min(frames - done, kAudioChunk);
                const uint8_t* in[32];
                for (int c = 0; c < 32; ++c) in[c] = (const uint8_t*)(c < channels && c < inChannels_ ? ch[c] + done : none);
                if (!queueAudio(in, n) || !encodeQueuedAudio(false)) return false;
                done += n;
            }
            return true;
        }
        if (channels == 6) {
            // a stereo file: the standard fold-down
            std::vector<float> l((size_t)std::max(0, frames)), r((size_t)std::max(0, frames));
            const float h = 0.70710678f;
            for (int k = 0; k < frames; ++k) {
                l[(size_t)k] = ch[0][k] + h * ch[2][k] + h * ch[4][k];
                r[(size_t)k] = ch[1][k] + h * ch[2][k] + h * ch[5][k];
            }
            return writeAudio(l.data(), r.data(), frames);
        }
        return writeAudio(ch[0], channels > 1 ? ch[1] : ch[0], frames);
    }
    if (finished_ || failed_) return false;
    if (!aenc_ || frames <= 0) return true;
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, kAudioChunk);
        const uint8_t* in[32];
        for (int c = 0; c < 32; ++c) in[c] = (const uint8_t*)(c < channels ? ch[c] + done : ch[0] + done);
        if (!queueAudio(in, n) || !encodeQueuedAudio(false)) return false;
        done += n;
    }
    return true;
}

bool FFWriter::finish(std::string* error)
{
    if (!finished_) {
        finished_ = true;
        if (headerWritten_) {
            // Whatever the encoders are still holding, then the container's own ending.
            if (!failed_ && venc_) encode(venc_.get(), vst_, nullptr);
            if (!failed_ && aenc_) {
                for (int i = 0; i < 16 && !failed_; ++i) {      // the tail of the sound still inside the converter
                    const int before = av_audio_fifo_size(fifo_.get());
                    if (!queueAudio(nullptr, 0) || av_audio_fifo_size(fifo_.get()) == before) break;
                }
                if (!failed_ && encodeQueuedAudio(true)) encode(aenc_.get(), ast_, nullptr);
            }
            // the last chapter ends where the file does
            if (oc_->nb_chapters > 0) {
                double seconds = 0;
                if (venc_) seconds = std::max(seconds, videoFrames_ * av_q2d(av_inv_q(venc_->framerate)));
                if (aenc_) seconds = std::max(seconds, (double)audioSamples_ / aenc_->sample_rate);
                AVChapter* last = oc_->chapters[oc_->nb_chapters - 1];
                last->end = std::max(last->start + 1, (int64_t)std::llround(seconds * 1000.0));
            }
            const int r = av_write_trailer(oc_.get());
            if (r < 0) fail("Finishing " + pathFileName(s_.path) + " failed", r);
        }
        if (oc_ && oc_->pb && !(oc_->oformat->flags & AVFMT_NOFILE)) {
            const int r = avio_closep(&oc_->pb);
            if (r < 0) fail("Closing " + pathFileName(s_.path) + " failed", r);
        }
        release();
    }
    if (failed_ && error) *error = firstError_;
    return !failed_;
}

}  // namespace

std::vector<EncoderMode> videoEncoderModes(const std::string& family, bool probe)
{
    std::vector<EncoderMode> out;
    const FamilyEncoders* f = familyOf(family);
    if (!f) return out;
    static std::mutex mutex;
    static std::map<std::string, bool> started;
    auto add = [&](const char* mode, const char* name) {
        if (!name) return;
        EncoderMode m;
        m.mode = mode;
        m.encoder = name;
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        m.present = codec && av_codec_is_encoder(codec);
        m.works = m.present && std::string(mode) == "cpu";
        if (m.present && probe && !m.works) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = started.find(name);
            if (it == started.end()) {
                bool ok = false;
                CodecPtr ctx(avcodec_alloc_context3(codec));
                if (ctx) {
                    EncodeSettings plain;
                    AVCodecContext* c = ctx.get();
                    c->width = 640;
                    c->height = 360;
                    c->pix_fmt = choosePixelFormat(codec, plain);
                    c->time_base = av_make_q(1, 30);
                    c->framerate = av_make_q(30, 1);
                    c->bit_rate = 2000000;
                    if (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
                    ok = avcodec_open2(c, codec, nullptr) >= 0;
                }
                it = started.emplace(name, ok).first;
            }
            m.works = it->second;
        }
        out.push_back(m);
    };
    for (const char* const* c = f->cpu; *c; ++c)
        if (avcodec_find_encoder_by_name(*c) || c == f->cpu) {
            add("cpu", *c);
            break;
        }
    add("nvenc", f->nvenc);
    add("qsv", f->qsv);
    add("amf", f->amf);
    add("mf", f->mf);
    return out;
}

bool videoFamilyAvailable(const std::string& family)
{
    const FamilyEncoders* f = familyOf(family);
    if (!f) return false;
    for (const char* const* c = f->cpu; *c; ++c)
        if (const AVCodec* codec = avcodec_find_encoder_by_name(*c))
            if (av_codec_is_encoder(codec)) return true;
    if (f->mf && avcodec_find_encoder_by_name(f->mf)) return true;
    // only a graphics card's encoder: if it starts here
    for (const EncoderMode& m : videoEncoderModes(family, true))
        if (m.works) return true;
    return false;
}

bool videoEncoderHasOption(const std::string& family, const std::string& option)
{
    const FamilyEncoders* f = familyOf(family);
    if (!f) return false;
    for (const char* const* c = f->cpu; *c; ++c)
        if (const AVCodec* codec = avcodec_find_encoder_by_name(*c))
            if (av_codec_is_encoder(codec) && codec->priv_class && av_opt_find((void*)&codec->priv_class, option.c_str(), nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ)) return true;
    return false;
}

bool audioFamilyAvailable(const std::string& family)
{
    static const std::map<std::string, std::vector<const char*>> encoders = {
        {"aac", {"aac", "aac_mf"}}, {"mp3", {"libmp3lame", "mp3_mf"}}, {"pcm", {"pcm_s16le"}}, {"flac", {"flac"}},
        {"vorbis", {"libvorbis", "vorbis"}}, {"opus", {"libopus", "opus"}}, {"wma", {"wmav2", "wmav1"}}, {"ac3", {"ac3", "ac3_mf"}},
        {"alac", {"alac"}}};
    auto it = encoders.find(family);
    if (it == encoders.end()) return false;
    for (const char* n : it->second)
        if (const AVCodec* codec = avcodec_find_encoder_by_name(n))
            if (av_codec_is_encoder(codec)) return true;
    return false;
}

std::unique_ptr<MediaWriter> createMediaWriter(const EncodeSettings& settings, std::string* error)
{
    std::unique_ptr<FFWriter> w(new FFWriter());
    std::string e;
    if (!w->open(settings, &e)) {
        if (e.empty()) e = "Cannot write " + pathFileName(settings.path) + ".";
        if (error) *error = e;
        return nullptr;
    }
    return std::unique_ptr<MediaWriter>(w.release());
}

}  // namespace reno
