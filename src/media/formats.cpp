// Which files count as media (see media.h): extensions for the import dialogs and Explorer, and what this FFmpeg can
// write. Reading does not depend on these lists.
#include "ffmpeg_common.h"

#include <algorithm>

namespace reno {

namespace {
const char* const kVideoExts[] = {
    "mp4", "m4v", "mov", "qt", "mkv", "webm", "avi", "divx", "wmv", "asf", "wtv", "dvr-ms", "flv", "f4v",
    "mpg", "mpeg", "mpe", "m1v", "m2v", "mpv", "m2p", "ps", "vob", "evo", "ts", "mts", "m2ts", "m2t", "tod", "mod",
    "3gp", "3g2", "ogv", "mxf", "dv", "dif", "gif", "apng", "y4m", "nut", "rm", "rmvb", "ivf", "obu",
    "h264", "264", "avc", "h265", "265", "hevc", "vc1", "amv", "insv", "lrv", nullptr};
const char* const kAudioExts[] = {
    "wav", "bwf", "rf64", "w64", "aif", "aiff", "aifc", "mp3", "mp2", "mpa", "m4a", "m4b", "aac", "adts", "flac",
    "ogg", "oga", "opus", "spx", "wma", "ac3", "eac3", "ec3", "dts", "amr", "awb", "ape", "wv", "tta", "mka", "caf",
    "au", "snd", "ra", "voc", "mpc", "tak", "shn", "dsf", "dff", "mlp", "thd", nullptr};
const char* const kPictureExts[] = {
    "png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "tif", "tiff", "gif", "webp", "tga", "dds", "exr", "hdr",
    "psd", "ico", "jp2", "j2k", "jpc", "avif", "heic", "heif", "dpx", "pcx", "ppm", "pgm", "pbm", "pam", "sgi",
    "qoi", "jxl", nullptr};

std::vector<std::string> listOf(const char* const* a)
{
    std::vector<std::string> v;
    for (; *a; ++a) v.push_back(*a);
    return v;
}
}  // namespace

const std::vector<std::string>& mediaExtensions(MediaKind kind)
{
    static const std::vector<std::string> video = listOf(kVideoExts), audio = listOf(kAudioExts), pictures = listOf(kPictureExts);
    static const std::vector<std::string> any = [] {
        std::vector<std::string> v = listOf(kVideoExts);
        for (const char* const* a : {kAudioExts, kPictureExts})
            for (; *a; ++a)
                if (std::find(v.begin(), v.end(), std::string(*a)) == v.end()) v.push_back(*a);
        return v;
    }();
    switch (kind) {
    case MediaKind::Video: return video;
    case MediaKind::Audio: return audio;
    case MediaKind::Picture: return pictures;
    default: return any;
    }
}

bool hasMediaExtension(const std::string& path, MediaKind kind)
{
    const std::string e = pathLowerExtension(path);
    if (e.empty()) return false;
    const std::vector<std::string>& v = mediaExtensions(kind);
    return std::find(v.begin(), v.end(), e) != v.end();
}

bool containerAvailable(const std::string& ext)
{
    if (ext.empty()) return false;
    const std::string name = "x." + toLower(ext);
    return av_guess_format(nullptr, name.c_str(), nullptr) != nullptr;
}

}  // namespace reno
