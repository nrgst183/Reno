// Reading and writing media files. The engine only sees these interfaces; the FFmpeg code behind them
// is in ffmpeg_reader.cpp and ffmpeg_writer.cpp.
#pragma once
#include "../core/base.h"
#include <atomic>
#include <functional>

namespace reno {

struct MediaInfo {
    std::string path;
    bool hasVideo = false, hasAudio = false;
    bool isStill = false;               // a frame file: one frame, shown for as long as wanted
    int width = 0, height = 0;          // as displayed (after rotation and non-square pixels are accounted for)
    double fps = 0;                     // frames per second (0 for stills)
    double duration = 0;                // seconds (0 for stills)
    double videoDuration = 0;           // seconds the picture lasts, from the file's time zero (0: no picture, or not known); the sound can last longer
    int sampleRate = 0, channels = 0;   // of the audio in the file
    std::string videoCodec, audioCodec; // names, for display
    std::string fieldOrder = "none";    // video: "none" (progressive), "upper" (top field first) or "lower" - as the file says
    // video: the shape of the file's pixels (width / height, as the file says; width and height above already have it in)
    // and how far (clockwise degrees) the file says to turn its frames: what Media Properties' overrides start from
    // (MediaItem::pixelAspectOverride, rotationOverride).
    double pixelAspect = 1.0;
    int rotation = 0;
    // Markers stored in the file itself (its chapters), as VEGAS's View > Event Media Markers shows them: seconds, name.
    std::vector<std::pair<double, std::string>> markers;
    int bitDepth = 8;                   // video: bits per sample of the file's frames (8; 10 or 12 for deeper video)
    bool hasAlpha = false;              // video: the frames carry transparency
    // audio: the file's own channels, in its order, by VEGAS's speaker names ("L", "R", "C", "LFE", "Ls", "Rs", "Lb",
    // "Rb"; "" where the file does not say), and its layout as FFmpeg names it ("5.1(side)", "stereo"...; ""
    // unknown).
    std::vector<std::string> channelNames;
    std::string channelLayout;
    // audio: how many samples (at sampleRate) at the start of the decoded sound are left out, as VEGAS leaves them
    // out: AAC's encoder delay in an MP4 (2112).
    int audioDelaySamples = 0;
};

// What the thread asking for frames wants of the readers it calls (set by the preview while rendering a frame; nested
// projects' readers see it too).
//   cancel: set by any thread when the frame is no longer wanted. A reader decoding a long stretch stops and returns
//           null; what it decoded is kept for later frames.
//   quick:  a frame that needs a long decode (a seek into a file with widely spaced key frames) may be the nearest
//           key frame instead. The reader then sets 'approximated' and stays there, so the exact frame decodes on from it.
struct MediaReadHints {
    const std::atomic<bool>* cancel = nullptr;
    bool quick = false;
    bool approximated = false;
    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
};
MediaReadHints& mediaReadHints();       // this thread's

// Sets this thread's hints for as long as it lives (and puts back what they were).
struct MediaReadHintsScope {
    MediaReadHints saved;
    MediaReadHintsScope(const std::atomic<bool>* cancel, bool quick) : saved(mediaReadHints())
    {
        MediaReadHints& h = mediaReadHints();
        h.cancel = cancel;
        h.quick = quick;
        h.approximated = false;
    }
    bool approximated() const { return mediaReadHints().approximated; }
    ~MediaReadHintsScope() { mediaReadHints() = saved; }
};

// One open media file. NOT safe to use from two threads at once: the engine opens one reader for
// frames and another for sound when it needs both at the same time.
class MediaReader {
public:
    virtual ~MediaReader() {}
    virtual const MediaInfo& info() const = 0;

    // The frame showing at 'seconds' from the start of the file, scaled to fit inside maxW x maxH while keeping its
    // shape (it is not padded: the result can be narrower or shorter than the box). 8-bit B,G,R,A, colour multiplied
    // by alpha, bottom row first (see Image in core/base.h).
    virtual ImagePtr videoFrame(double seconds, int maxW, int maxH) = 0;

    // 'frames' samples of stereo 32-bit float sound at 'sampleRate', starting 'start' samples (at that rate) from the
    // start of the file.
    virtual bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) = 0;

    // The same in 'channels' channels (out[0 .. channels)): 2 is audio(); 6 is 5.1 (L R C LFE Ls Rs) - a 5.1 file's
    // own channels, anything else mixed (or spread) to 5.1 the standard way.
    virtual bool audioChannels(int64_t start, int frames, int sampleRate, int channels, float* const* out)
    {
        if (channels < 2) return false;
        for (int c = 2; c < channels; ++c)
            for (int k = 0; k < frames; ++k) out[c][k] = 0.f;
        return audio(start, frames, sampleRate, out[0], out[1]);
    }

    // The file's OWN channels rather than its stereo mix: 'count' of them (1 or 2) from channel 'first' (0-based, in
    // the file's order: MediaInfo::channelNames), at 'sampleRate'.
    virtual bool audioPick(int64_t start, int frames, int sampleRate, int first, int count, float* left, float* right)
    {
        if (first < 0) return audio(start, frames, sampleRate, left, right);
        const int have = info().channels > 0 ? info().channels : 2;
        if (first >= 2 || first >= have) {
            for (int k = 0; k < frames; ++k) left[k] = right[k] = 0.f;
            return false;
        }
        const bool ok = audio(start, frames, sampleRate, left, right);
        if (count >= 2 && first == 0 && have >= 2) return ok;
        for (int k = 0; k < frames; ++k) {
            const float v = first == 0 ? left[k] : right[k];
            left[k] = right[k] = v;
        }
        return ok;
    }

    // videoFrame(), and the stretch of times it shows for: every time in [*from, *until) gives this same frame (MediaPool
    // shares frames between its readers by that, so that a frame decoded once is used by every thread that wants it).
    // *from > *until says nothing is known (readers that cannot tell leave it so), and the frame is then good for 'seconds' only.
    virtual ImagePtr videoFrameSpan(double seconds, int maxW, int maxH, double* from, double* until)
    {
        *from = 1.0;
        *until = 0.0;
        return videoFrame(seconds, maxW, maxH);
    }

    // Frames the reader decoded on its way to the last frame it gave and still holds (a seek back to a key frame
    // decodes a stretch of them): given to 'put' (the times each is good for, as videoFrameSpan, and the picture at
    // the size videoFrameSpan makes for maxW x maxH), at most 'most' of them, each only once.
    virtual int harvestKept(int maxW, int maxH, int most, const std::function<void(double from, double until, const ImagePtr& img)>& put)
    {
        (void)maxW; (void)maxH; (void)most; (void)put;
        return 0;
    }

    // The same frame as videoFrame() gives, in 'depth'. Float (R, G, B, A, 0..1, premultiplied) is made straight
    // from the file's own samples where they have more than 8 bits (10- and 12-bit video), so a 32-bit project keeps
    // their precision; readers that cannot do better give the 8-bit frame converted.
    virtual ImagePtr videoFrameInDepth(double seconds, int maxW, int maxH, PixelDepth depth)
    {
        ImagePtr img = videoFrame(seconds, maxW, maxH);
        return (img && img->depth != depth) ? img->converted(depth) : img;
    }

    // What videoFrame() does with interlaced frames: 0 shows them as they are (the default), 1 blends the two fields
    // together, 2 keeps the first field and fills the other's lines in from it (VEGAS's deinterlace methods).
    virtual void setDeinterlace(int mode) { (void)mode; }
    // Media Properties' Field order override: "" as the file says; "none" (the frames are progressive, whatever the
    // file says: nothing is deinterlaced), "upper" or "lower" (interlaced, that field first).
    virtual void setFieldOrder(const std::string& order) { (void)order; }

    // How much work videoFrame(seconds, maxW, maxH) would be now, so that a caller holding several readers of one
    // file can ask the one that is nearest.
    virtual int videoCost(double seconds, int maxW, int maxH) { (void)seconds; (void)maxW; (void)maxH; return 2; }

    // The graphics-card API this reader decodes its frames on ("d3d11va", "cuda"...), empty when it decodes in software
    // (also after it fell back to software).
    virtual std::string hardwareDecoder() const { return std::string(); }
};

// Opens a file (video, audio or a frame). Null with *error set if it cannot be read.
std::unique_ptr<MediaReader> openMedia(const std::string& path, std::string* error = nullptr);

struct EncodeSettings {
    std::string path;                   // the container is chosen from the extension (.mp4, .mov, .mkv, .avi, .webm, .wav, .mp3 ...)
    bool video = true, audio = true;
    int width = 1920, height = 1080;
    int fpsNum = 30, fpsDen = 1;
    int sampleRate = 48000;
    int videoKbps = 12000, audioKbps = 192;
    std::string videoCodec;             // an FFmpeg encoder name, or empty to pick the best one this build of FFmpeg has for the container
    std::string audioCodec;             // likewise

    // ---- what a Render As template sets (File > Render As). The defaults give what the fields above alone give.
    // The kind of frame compression, when videoCodec is empty: "h264", "hevc", "prores", "dnxhd", "mpeg2", "dv",
    // "mpeg4", "mjpeg", "wmv", "vp9", "rawvideo", "utvideo", "png", "jpeg", "tiff", "bmp" ("" = the container's usual one).
    std::string videoFamily;
    // Which encoder makes it: "auto" (the usual order), "cpu" (software only), "nvenc" (NVIDIA), "qsv" (Intel Quick
    // Sync), "amf" (AMD), "mf" (Windows Media Foundation).
    std::string encodeMode = "auto";
    // The kind of sound compression, when audioCodec is empty: "aac", "mp3", "pcm", "flac", "vorbis", "opus", "wma", "ac3".
    std::string audioFamily;
    int outSampleRate = 0;              // the file's sample rate (0: sampleRate, which is the rate of the sound given)
    int audioChannels = 2;              // 1 (mixed to mono), 2 or 6 (5.1: L R C LFE Ls Rs, given with writeAudioChannels); with PCM any
                                        // count up to 32 (Render As's multichannel mapping)
    int audioBits = 16;                 // PCM: 16, 24 or 32 (floating point)
    std::string rateControl = "vbr";    // video: "vbr" (average videoKbps, peaks up to maxKbps), "cbr", "quality" (constant quality)
    int maxKbps = 0;                    // vbr: the highest rate (0: the encoder's own choice)
    int quality = 23;                   // "quality": 0 (best) .. 51, as x264's CRF
    int pass = 0;                       // two-pass encoding: 1 = the first pass (statistics only), 2 = the second; 0 = one pass
    std::string passLog;                // two-pass: where the statistics are kept between the passes
    std::string profile;                // H.264/HEVC: "baseline", "main", "high", "main10"; ProRes: "proxy", "lt", "standard", "hq", "4444"
    std::string level;                  // H.264/HEVC level, e.g. "4.2" ("" = automatic)
    double keyframeSeconds = 2.0;       // a key frame at least this often (0: the encoder's own choice)
    int bFrames = -1;                   // B-frames between reference frames (-1: the encoder's own choice)
    std::string fieldOrder = "none";    // what the file says the frames are: "none" (progressive), "upper", "lower"
    double pixelAspect = 1.0;           // what the file says the pixels' shape is
    // Frames given at another size than width x height are scaled to it: fitted inside with black bars keeping
    // their shape (letterboxed; inPixelAspect and pixelAspect taken into account), or stretched to fill it.
    int inWidth = 0, inHeight = 0;      // the size the frames come in (0: width x height)
    double inPixelAspect = 1.0;
    bool stretch = false;
    bool alpha = false;                 // keep the frames' transparency where the format can (ProRes 4444, PNG, TIFF, uncompressed)
    // Chapters (Render As's "Save project markers in media file") and text tags (title, artist, comment...).
    std::vector<std::pair<double, std::string>> chapters;      // seconds into the file, name
    std::vector<std::pair<std::string, std::string>> metadata;
    // A 360° video (Project Properties' 360° output): the frame stream marked as equirectangular spherical video
    // (MP4/MOV's sv3d box, Matroska/WebM's projection), so players show it as 360°.
    bool spherical = false;
    // Encoder options in FFmpeg's words ("preset=slow tune=film", separated by spaces; "x264-params" / "x265-params" are
    // added to the ones Reno sets) given to the frame encoder after Reno's own (the FFmpeg templates)
    std::string encoderOptions;
    // HDR: how the frames are coded - transfer "pq" (SMPTE ST 2084) or "hlg" (ARIB STD-B67) with the primaries
    // "bt2020" or "p3d65"; "" is SDR (the colour tags stay BT.709/601).
    std::string hdrTransfer, hdrPrimaries;
    bool mastering = false;
    double mdRed[2] = {0.708, 0.292}, mdGreen[2] = {0.170, 0.797}, mdBlue[2] = {0.131, 0.046}, mdWhite[2] = {0.3127, 0.3290};
    double mdMaxNits = 1000.0, mdMinNits = 0.005;
    int maxCll = 0, maxFall = 0;
};

// One way of making a kind of frame compression (EncodeSettings::videoFamily) with this FFmpeg: for Render As's
// Encode mode list.
struct EncoderMode {
    std::string mode;                   // "cpu", "nvenc", "qsv", "amf", "mf"
    std::string encoder;                // the FFmpeg encoder's name, e.g. "h264_nvenc"
    bool present = false;               // this FFmpeg has it
    bool works = false;                 // it starts: a software encoder if present; the others only when 'probe' was asked for (tried on a small frame)
};
std::vector<EncoderMode> videoEncoderModes(const std::string& family, bool probe);

class MediaWriter {
public:
    virtual ~MediaWriter() {}
    // Frames in order, each width x height, 8-bit BGRA bottom row first. False on error.
    virtual bool writeVideo(const Image& frame) = 0;
    // The sound that goes with them, in order, any block size.
    virtual bool writeAudio(const float* left, const float* right, int frames) = 0;
    // The same in 'channels' channels (2, or 6: L R C LFE Ls Rs). A file of other channels gets them mixed to its own.
    virtual bool writeAudioChannels(const float* const* ch, int channels, int frames)
    {
        return writeAudio(ch[0], channels > 1 ? ch[1] : ch[0], frames);
    }
    // Flushes the encoders and closes the file. Must be called; false if anything failed (*error says what).
    virtual bool finish(std::string* error = nullptr) = 0;
    // What was chosen, for the log: e.g. "mp4: h264_mf 1920x1080 30 fps, aac 48000 Hz".
    virtual std::string describe() const = 0;
};

// Null with *error set if the file cannot be created or no usable encoder exists.
std::unique_ptr<MediaWriter> createMediaWriter(const EncodeSettings& settings, std::string* error = nullptr);

// ---- image sequences (VEGAS's "Open still image sequence"): numbered frames (shot_0001.png, shot_0002.png ...) read
// as one video at a chosen frame rate. The project keeps the first frame's path and the rate
// (MediaItem::sequenceFps); openMedia() is given the spec below, which reads the frames from that one in a row until
// a number is missing.
int imageSequenceLength(const std::string& firstFile);
// The text openMedia() reads an image sequence by, and back.
std::string imageSequenceSpec(const std::string& firstFile, double fps);
bool parseImageSequenceSpec(const std::string& spec, std::string& firstFile, double& fps);
// FFmpeg's pattern for the frames ("shot_%04d.png") and the first number, or false.
bool imageSequencePattern(const std::string& firstFile, std::string& pattern, int& firstNumber);

// Whether Intel Quick Sync encoders may be used (Preferences > General: "Enable QSV Encoding and Decoding"; on by default):
// cleared, no render mode or Automatic choice tries a *_qsv encoder.
void setQsvEncodingAllowed(bool on);
bool qsvEncodingAllowed();

// How many threads each video decoder opened from now on uses: 0 (the default) automatic - one per processor for the first
// few decoders, fewer for many at once - or this many (Preferences > Reno > Performance).
void setVideoDecoderThreadLimit(int n);
int videoDecoderThreadLimit();

// ---- hardware (graphics card) video decoding (hwdecode.cpp). VEGAS decodes AVC/HEVC on the card
// (Preferences > File I/O); so does Reno, through FFmpeg's hwaccels: D3D11VA and DXVA2 (any card) or CUDA/NVDEC on
// Windows, VAAPI/VDPAU on Linux, VideoToolbox on macOS.
enum class HwDecodeMode { Off, Auto };
void setHardwareDecoding(HwDecodeMode mode, const std::string& api = std::string());
HwDecodeMode hardwareDecoding();
std::string hardwareDecodingApi();
// At most this many files decode on the card at once (cards run a limited number of decoder sessions); the others use the
// processor. Default 8.
void setHardwareDecoderLimit(int n);
int hardwareDecoderLimit();
// The APIs this FFmpeg has and this platform may try, in the usual order (names as setHardwareDecoding takes them).
std::vector<std::string> hardwareDecodeApis();

// ---- video proxies (VEGAS's Project Media > Create Video Proxy). A proxy is a small, easily decoded copy of a video
// file's frame, kept next to it as "<file>.sfvp0" (VEGAS's own name for its proxies, so proxies VEGAS made are used
// too): the Video Preview shows it instead of the original when its quality is below Best, which keeps big or heavily
// compressed files (4K, HEVC, long-GOP camera files) playing smoothly; renders always use the original.
void setVideoProxyFolder(const std::string& folder);
std::string videoProxyFolder();
std::string videoProxyPath(const std::string& mediaPath);
// (False for a proxy older than the file it was made from: the file was rendered or edited again under the same name.)
bool hasVideoProxy(const std::string& mediaPath);
// How a proxy is made. The defaults are what Reno uses (tuned for being made fast and read fast, see proxy.cpp); the
// fields exist for the automatic proxies (one encoder thread) and for the measuring programs in tests/.
struct VideoProxyOptions {
    int maxWidth = 960, maxHeight = 540;    // the original's shape inside this box, never enlarged
    double keyframeSeconds = 0.5;           // a key frame this often (a read that seeks decodes at most this much)
    int quality = 30;                       // x264's CRF
    std::string encoderOptions = "preset=ultrafast tune=fastdecode";   // FFmpeg's words (see EncodeSettings)
    int encoderThreads = 0;                 // 0: the encoder's own choice (all processors, or Preferences' limit)
    std::string outputPath;                 // empty: videoProxyPath(mediaPath); the finished file is renamed from "<it>.part.mp4"
    double fromSeconds = 0, toSeconds = 0;  // only this part of the original (0, 0: all of it; for measuring)
    bool announce = true;                   // tell readers proxies changed when done (false for files that are not proxies)
};
// Makes the proxy of 'mediaPath' (replacing one that is there). 'progress' is told how far it has got (0..1) and may
// return false to stop (nothing is left behind then). False with *error set if it failed or was stopped.
bool createVideoProxy(const std::string& mediaPath, const std::function<bool(double)>& progress, std::string* error = nullptr);
bool createVideoProxy(const std::string& mediaPath, const VideoProxyOptions& options, const std::function<bool(double)>& progress, std::string* error = nullptr);

// Whether a proxy would make the file's preview better, from what the file says without decoding it.
struct VideoProxyAssessment {
    bool worth = false;
    std::string reason;                     // why not (or a short note), for logs and tests
    double keyframeSeconds = 0;             // the longest spacing of key frames found (0: not known)
    int keyframes = 0;                      // how many the index lists (0: no index)
};
bool isLongGopVideoCodec(const std::string& codec);
VideoProxyAssessment assessVideoProxy(const std::string& mediaPath, const MediaInfo& info);
// Whether the preview is playing right now (Playback tells it): the background proxy maker waits while it is.
void setPreviewPlaying(bool playing);
bool previewPlaying();
// Counts proxies made (and removed) since the program started, so that readers can tell when to look again.
uint64_t videoProxyGeneration();
void videoProxiesChanged();

// ---- which files are media. The extensions (lower case, no dot) the import dialogs, the Explorer
// and the Device Explorer offer. They only decide what is SHOWN: openMedia() reads whatever FFmpeg makes sense of,
// whatever its name, so a file with another extension (or none) still imports when picked with "All Files" or dropped.
enum class MediaKind { Video, Audio, Picture, Any };
const std::vector<std::string>& mediaExtensions(MediaKind kind);
bool hasMediaExtension(const std::string& path, MediaKind kind = MediaKind::Any);

// What this FFmpeg can write (Render As offers only the templates it can make).
bool videoFamilyAvailable(const std::string& family);
// Whether the software encoder of a kind of frame compression has the private option (libx264's "avcintra-class").
bool videoEncoderHasOption(const std::string& family, const std::string& option);
bool audioFamilyAvailable(const std::string& family);
bool containerAvailable(const std::string& ext);

// The project a rendered file names (Render As's "Save project as path reference in rendered media": a "Project:
// <path>" line in the file's comment), or "" - for VEGAS's /OPENPRJ.
std::string mediaProjectReference(const std::string& path);

// Version of the FFmpeg libraries in use, for the About box and logs.
std::string mediaBackendVersion();

}  // namespace reno
