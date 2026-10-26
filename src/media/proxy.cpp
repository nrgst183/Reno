// Video proxies (see media.h): made with the ordinary reader and writer, read back as ordinary media.
#include "media.h"
#include "ffmpeg_common.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace reno {

namespace {
std::atomic<uint64_t> gGeneration{1};

std::filesystem::path fsPath(const std::string& p)
{
#ifdef _WIN32
    return std::filesystem::path(widen(p));
#else
    return std::filesystem::path(p);
#endif
}
}  // namespace

namespace {
std::mutex gFolderMutex;
std::string gProxyFolder;

// "<8 hex digits of the media's folder>_<file name>.sfvp0" in the proxy folder (two files of one name in two folders differ)
std::string folderProxyPath(const std::string& folder, const std::string& mediaPath)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : pathDir(mediaPath)) h = (h ^ c) * 16777619u;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x_", (unsigned)h);
    std::string name = pathFileName(mediaPath);
    if (name.size() > 120) {
        uint32_t g = 2166136261u;
        for (unsigned char c : name) g = (g ^ c) * 16777619u;
        char tail[16];
        std::snprintf(tail, sizeof tail, "~%08x", (unsigned)g);
        size_t keep = 100;
        while (keep > 0 && (static_cast<unsigned char>(name[keep]) & 0xC0) == 0x80) --keep;
        name = name.substr(0, keep) + tail;
    }
    return pathJoin(folder, std::string(buf) + name + ".sfvp0");
}
}  // namespace

void setVideoProxyFolder(const std::string& folder)
{
    std::lock_guard<std::mutex> lk(gFolderMutex);
    if (gProxyFolder == folder) return;
    gProxyFolder = folder;
    gGeneration.fetch_add(1);
}
std::string videoProxyFolder()
{
    std::lock_guard<std::mutex> lk(gFolderMutex);
    return gProxyFolder;
}

std::string videoProxyPath(const std::string& mediaPath)
{
    const std::string folder = videoProxyFolder();
    if (!folder.empty() && !mediaPath.empty()) {
        const std::string there = folderProxyPath(folder, mediaPath);
        // a proxy made before the folder was chosen, or by VEGAS, beside the media is used while the folder has none.
        if (fileExists(there) || !fileExists(mediaPath + ".sfvp0")) return there;
    }
    return mediaPath + ".sfvp0";
}

bool hasVideoProxy(const std::string& mediaPath)
{
    if (mediaPath.empty()) return false;
    const std::string proxy = videoProxyPath(mediaPath);
    if (!fileExists(proxy)) return false;
    // a proxy older than the file it was made from is of an earlier version of it (the file was rendered or edited again under the
    // same name): not one to use, and one to make again
    std::error_code a, b;
    const auto pt = std::filesystem::last_write_time(fsPath(proxy), a), mt = std::filesystem::last_write_time(fsPath(mediaPath), b);
    if (!a && !b && pt + std::chrono::seconds(2) < mt) return false;
    return true;
}

namespace {
std::atomic<bool> gPreviewPlaying{false};
}
void setPreviewPlaying(bool playing) { gPreviewPlaying = playing; }
bool previewPlaying() { return gPreviewPlaying.load(); }

uint64_t videoProxyGeneration() { return gGeneration.load(); }
void videoProxiesChanged() { gGeneration.fetch_add(1); }

bool createVideoProxy(const std::string& mediaPath, const std::function<bool(double)>& progress, std::string* error)
{
    return createVideoProxy(mediaPath, VideoProxyOptions(), progress, error);
}

bool createVideoProxy(const std::string& mediaPath, const VideoProxyOptions& opt, const std::function<bool(double)>& progress, std::string* error)
{
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    std::string err;
    std::unique_ptr<MediaReader> in = openMedia(mediaPath, &err);
    if (!in) return fail(err.empty() ? "Cannot read " + pathFileName(mediaPath) + "." : err);
    const MediaInfo& mi = in->info();
    if (!mi.hasVideo || mi.isStill) return fail(pathFileName(mediaPath) + " has no moving picture to make a proxy of.");
    if (!(mi.fps > 0.5) || !(mi.duration > 0) || mi.width <= 0 || mi.height <= 0)
        return fail("The frame rate or length of " + pathFileName(mediaPath) + " is not known.");
    // the size: the original's shape inside 960 x 540 (never larger than it), even numbers for the encoder
    const double k = std::min(1.0, std::min((double)std::max(2, opt.maxWidth) / mi.width, (double)std::max(2, opt.maxHeight) / mi.height));
    int w = std::max(2, (int)std::lround(mi.width * k)) & ~1, h = std::max(2, (int)std::lround(mi.height * k)) & ~1;
    EncodeSettings es;
    const std::string final = opt.outputPath.empty() ? videoProxyPath(mediaPath) : opt.outputPath, part = final + ".part.mp4";
    es.path = part;
    {
        std::error_code mk;
        std::filesystem::create_directories(fsPath(pathDir(final)), mk);
    }
    es.audio = false;
    es.width = w;
    es.height = h;
    // the frame rate as a fraction (29.97 -> 30000/1001)
    const double fps = mi.fps;
    if (std::fabs(fps * 1001 / 1000 - std::round(fps * 1001 / 1000)) < 1e-3 && std::fabs(fps - std::round(fps)) > 1e-3) {
        es.fpsNum = (int)std::lround(fps * 1001 / 1000) * 1000;
        es.fpsDen = 1001;
    } else if (std::fabs(fps - std::round(fps)) < 1e-3) {
        es.fpsNum = (int)std::lround(fps);
        es.fpsDen = 1;
    } else {
        es.fpsNum = (int)std::lround(fps * 1000);
        es.fpsDen = 1000;
    }
    es.videoFamily = "h264";
    es.encodeMode = "cpu";
    es.rateControl = "quality";
    es.quality = opt.quality;
    es.keyframeSeconds = opt.keyframeSeconds;
    es.bFrames = 0;
    es.encoderOptions = opt.encoderOptions;
    if (opt.encoderThreads > 0) es.encoderOptions += (es.encoderOptions.empty() ? "" : " ") + strfmt("threads=%d", opt.encoderThreads);
    es.stretch = true;
    // H.264 from the processor first (libx264 / OpenH264).
    std::unique_ptr<MediaWriter> out = createMediaWriter(es, &err);
    if (!out) {
        const std::string first = err;
        es.encodeMode = "auto";
        err.clear();
        out = createMediaWriter(es, &err);
        if (!out) {
            es.videoFamily = "mpeg4";
            es.encodeMode = "cpu";
            err.clear();
            out = createMediaWriter(es, &err);
        }
        if (!out) return fail(first.empty() ? "Cannot make " + pathFileName(part) + "." : first + (err.empty() ? std::string() : " MPEG-4: " + err));
    }
    const double rate = (double)es.fpsNum / es.fpsDen;
    const double from = std::max(0.0, std::min(opt.fromSeconds, mi.duration)), to = opt.toSeconds > from ? std::min(opt.toSeconds, mi.duration) : mi.duration;
    const int64_t first = (int64_t)std::floor(from * rate + 1e-6);
    const int64_t frames = std::max<int64_t>(1, (int64_t)std::floor(to * rate + 1e-6) - first);
    bool ok = true, stopped = false;
    ImagePtr last;
    // frame by frame in order from the first: the reader decodes straight through, it never seeks after the first frame.
    for (int64_t f = 0; f < frames; ++f) {
        // the middle of each frame, as the engine asks for frames
        ImagePtr img = in->videoFrame((first + f + 0.5) / rate, w, h);
        if (!img) img = last;
        if (!img) continue;
        if (img->w != w || img->h != h || img->depth != PixelDepth::Byte) {
            ImagePtr fit = Image::make(w, h, PixelDepth::Byte, false);
            if (!fit) { ok = false; break; }
            for (int y = 0; y < h; ++y) {
                const int sy = std::min(img->h - 1, y * img->h / h);
                for (int x = 0; x < w; ++x) {
                    const int sx = std::min(img->w - 1, x * img->w / w);
                    uint8_t* d = fit->row(y) + x * 4;
                    if (img->depth == PixelDepth::Byte) std::memcpy(d, img->row(sy) + sx * 4, 4);
                    else {
                        const float* s = (const float*)img->row(sy) + sx * 4;
                        auto q = [](float v) { return (uint8_t)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f); };
                        d[0] = q(s[2]); d[1] = q(s[1]); d[2] = q(s[0]); d[3] = q(s[3]);
                    }
                }
            }
            img = fit;
        }
        last = img;
        if (!out->writeVideo(*img)) { ok = false; err = "Writing the proxy failed."; break; }
        if (progress && (f % 8 == 0 || f + 1 == frames) && !progress((double)(f + 1) / frames)) { stopped = true; break; }
    }
    std::string ferr;
    const bool finished = out->finish(&ferr);
    out.reset();
    std::error_code ec;
    if (!ok || stopped || !finished) {
        std::filesystem::remove(fsPath(part), ec);
        return fail(stopped ? std::string("Stopped.") : (!ferr.empty() ? ferr : err));
    }
    std::filesystem::remove(fsPath(final), ec);
    std::filesystem::rename(fsPath(part), fsPath(final), ec);
    if (ec) {
        std::filesystem::remove(fsPath(part), ec);
        return fail("Cannot put the proxy in place as " + pathFileName(final) + ".");
    }
    if (opt.announce) videoProxiesChanged();
    return true;
}

bool isLongGopVideoCodec(const std::string& codec)
{
    for (const char* c : {"h264", "hevc", "h265", "mpeg2video", "mpeg2", "mpeg1video", "mpeg4", "vp9", "vp8", "av1", "vc1", "wmv3"})
        if (codec == c) return true;
    return false;
}

VideoProxyAssessment assessVideoProxy(const std::string& mediaPath, const MediaInfo& mi)
{
    VideoProxyAssessment a;
    if (!mi.hasVideo || mi.isStill || !(mi.duration > 0) || mi.width <= 0 || mi.height <= 0) {
        a.reason = "no moving picture";
        return a;
    }
    if (!isLongGopVideoCodec(mi.videoCodec)) {
        a.reason = "every frame is a key frame (" + mi.videoCodec + ")";
        return a;
    }
    std::string seqFirst;
    double seqFps = 0;
    if (parseImageSequenceSpec(mediaPath, seqFirst, seqFps)) {
        a.reason = "image sequence";
        return a;
    }
    // the key frames, from the index the container keeps (read when the file is opened, nothing is decoded)
    using namespace ff;
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, mediaPath.c_str(), nullptr, nullptr) >= 0 && fc) {
        InputPtr in(fc);
        int vi = -1;
        for (unsigned i = 0; i < fc->nb_streams; ++i)
            if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(fc->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                vi = (int)i;
                break;
            }
        if (vi >= 0) {
            AVStream* st = fc->streams[vi];
            const int n = avformat_index_get_entries_count(st);
            double prev = 0, longest = 0;
            int keys = 0;
            for (int i = 0; i < n; ++i) {
                const AVIndexEntry* e = avformat_index_get_entry(st, i);
                if (!e || !(e->flags & AVINDEX_KEYFRAME)) continue;
                const double t = (double)e->timestamp * av_q2d(st->time_base);
                if (keys > 0) longest = std::max(longest, t - prev);
                prev = t;
                ++keys;
            }
            if (keys > 0) {
                longest = std::max(longest, mi.duration - prev);
                a.keyframes = keys;
                a.keyframeSeconds = longest;
            }
        }
    }
    const bool small = mi.width <= 960 && mi.height <= 540;
    if (small && a.keyframes > 0 && a.keyframeSeconds <= 0.6) {
        a.reason = "small, and a key frame at least every 0.6 s";
        return a;
    }
    a.worth = true;
    a.reason = a.keyframes > 0 ? strfmt("key frames up to %.1f s apart", a.keyframeSeconds) : std::string("long-GOP codec, key frames not known");
    return a;
}

}  // namespace reno
