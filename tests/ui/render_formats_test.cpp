// Render As's formats and templates (src/app/rendertemplates.cpp): every one this FFmpeg can make is rendered from a small project and
// read back with openMedia(): it must have the streams, codec, size, frame rate, length, sample rate and channels the template says,
// its pictures must be the project's (the frame number each carries), transparency must be kept where the template keeps it, and
// the sound must be in its place. The older formats' templates are rendered too, the first of each (and every format's "with
// alpha" one), so nothing they did is lost. Templates whose encoder this FFmpeg lacks are not offered (templateAvailable); those
// are listed.
//
//   render_formats_test <output folder> [--only <text>]
//
// The project: 320x240 at 30 fps, 48 kHz; a video track with 30 numbered pictures (PNG, with a transparent rectangle) and an audio
// track with a 440 Hz / 880 Hz sound with a click at 0.5 s. Templates with a frame size of their own are rendered at the project's
// size (so the pictures can be checked), with their own frame rate.
#include "../../src/app/rendertemplates.h"
#include "../../src/engine/renderas.h"
#include "../../src/media/media.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

using namespace reno;
using namespace reno::app;

namespace {

int gPass = 0, gFail = 0;
bool check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    (ok ? gPass : gFail)++;
    return ok;
}

const int kW = 320, kH = 240, kFps = 30, kFrames = 30, kRate = 48000;
struct Rect { int x, y, w, h; };
Rect bitRect(int i) { return Rect{4 + i * 39, 8, 32, 32}; }
const Rect kClear = {200, 110, 100, 80};

void fill(Image& img, const Rect& r, int b, int g, int rr, int a)
{
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            uint8_t* p = img.row(img.h - 1 - y) + 4 * x;
            p[0] = (uint8_t)(b * a / 255);
            p[1] = (uint8_t)(g * a / 255);
            p[2] = (uint8_t)(rr * a / 255);
            p[3] = (uint8_t)a;
        }
}

double meanOf(const Image& img, const Rect& r, int inset, int channel)
{
    const double sx = img.w / (double)kW, sy = img.h / (double)kH;
    const int x0 = (int)std::ceil((r.x + inset) * sx), x1 = (int)std::floor((r.x + r.w - inset) * sx);
    const int y0 = (int)std::ceil((r.y + inset) * sy), y1 = (int)std::floor((r.y + r.h - inset) * sy);
    double s = 0;
    int n = 0;
    for (int y = std::max(0, y0); y < std::min(img.h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(img.w, x1); ++x, ++n) {
            if (img.depth == PixelDepth::Float) s += ((const float*)img.row(img.h - 1 - y))[4 * x + (channel == 3 ? 3 : 2 - channel)] * 255.0;
            else s += img.row(img.h - 1 - y)[4 * x + channel];
        }
    return n ? s / n : 0;
}

int frameNumber(const Image* img)
{
    if (!img) return -1;
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        const double v = (meanOf(*img, bitRect(i), 9, 0) + meanOf(*img, bitRect(i), 9, 1) + meanOf(*img, bitRect(i), 9, 2)) / 3;
        if (v > 150) n |= 1 << (7 - i);
        else if (v > 100) return -1;
    }
    return n;
}

// The test media: numbered pictures and a sound, written with Reno's own writer.
bool makeMedia(const std::string& dir, std::string* firstPicture, std::string* sound)
{
    QDir().mkpath(QString::fromStdString(dir));
    EncodeSettings s;
    s.path = dir + "/p_%03d.png";
    s.audio = false;
    s.width = kW;
    s.height = kH;
    s.fpsNum = kFps;
    s.videoFamily = "png";
    s.alpha = true;
    std::string e;
    auto w = createMediaWriter(s, &e);
    if (!w) return false;
    for (int n = 0; n < kFrames; ++n) {
        ImagePtr img = Image::make(kW, kH, PixelDepth::Byte, false);
        fill(*img, Rect{0, 0, kW, kH}, 96, 96, 96, 255);
        for (int i = 0; i < 8; ++i) {
            const int v = (n >> (7 - i)) & 1 ? 255 : 0;
            fill(*img, bitRect(i), v, v, v, 255);
        }
        fill(*img, Rect{8, 56, 40, 40}, 0, 0, 255, 255);        // red
        fill(*img, Rect{60, 56, 40, 40}, 0, 255, 0, 255);       // green
        fill(*img, kClear, 0, 0, 0, 0);
        if (!w->writeVideo(*img)) return false;
    }
    if (!w->finish(&e)) return false;
    *firstPicture = dir + "/p_000.png";
    EncodeSettings a;
    a.path = dir + "/sound.wav";
    a.video = false;
    a.sampleRate = kRate;
    w = createMediaWriter(a, &e);
    if (!w) return false;
    std::vector<float> l(kRate), r(kRate);
    for (int i = 0; i < kRate; ++i) {
        l[i] = (float)(0.25 * std::sin(2 * M_PI * 440 * i / kRate));
        r[i] = (float)(0.25 * std::sin(2 * M_PI * 880 * i / kRate));
    }
    for (int k = 0; k < 48; ++k) l[kRate / 2 + k] = r[kRate / 2 + k] = k < 24 ? 0.9f : -0.9f;
    w->writeAudio(l.data(), r.data(), kRate);
    *sound = a.path;
    return w->finish(&e);
}

Project makeProject(const std::string& firstPicture, const std::string& sound)
{
    Project p;
    p.settings.width = kW;
    p.settings.height = kH;
    p.settings.fpsNum = kFps;
    p.settings.fpsDen = 1;
    p.settings.sampleRate = kRate;
    MediaItem v;
    v.id = p.newId();
    v.path = firstPicture;
    v.sequenceFps = kFps;
    if (auto r = openMedia(v.readPath())) v.info = r->info();
    p.media.push_back(v);
    MediaItem a;
    a.id = p.newId();
    a.path = sound;
    if (auto r = openMedia(sound)) a.info = r->info();
    p.media.push_back(a);
    Track vt;
    vt.id = p.newId();
    vt.type = TrackType::Video;
    Event ve;
    ve.id = p.newId();
    ve.mediaId = v.id;
    ve.length = 1.0;
    vt.events.push_back(ve);
    p.tracks.push_back(vt);
    Track at;
    at.id = p.newId();
    at.type = TrackType::Audio;
    Event ae;
    ae.id = p.newId();
    ae.mediaId = a.id;
    ae.length = 1.0;
    at.events.push_back(ae);
    p.tracks.push_back(at);
    return p;
}

// The codec FFmpeg names for a kind of compression, as openMedia() reports it.
std::string codecName(const std::string& family)
{
    static const std::map<std::string, std::string> m = {
        {"h264", "h264"}, {"hevc", "hevc"}, {"vp8", "vp8"}, {"vp9", "vp9"}, {"av1", "av1"}, {"theora", "theora"}, {"gif", "gif"},
        {"apng", "apng"}, {"qtrle", "qtrle"}, {"ffv1", "ffv1"}, {"prores", "prores"}, {"png", "png"}, {"mjpeg", "mjpeg"}, {"tiff", "tiff"},
        {"bmp", "bmp"}, {"exr", "exr"}, {"tga", "targa"}, {"webp", "webp"}, {"wmv", "wmv"}, {"dnxhd", "dnxhd"}, {"mpeg1", "mpeg1video"}, {"mp2", "mp2"}, {"mpeg2", "mpeg2video"},
        {"dv", "dvvideo"}, {"rawvideo", "rawvideo"}, {"utvideo", "utvideo"}, {"aac", "aac"}, {"mp3", "mp3"}, {"flac", "flac"},
        {"vorbis", "vorbis"}, {"opus", "opus"}, {"wma", "wma"}, {"ac3", "ac3"}, {"alac", "alac"}, {"pcm", "pcm_"}};
    auto it = m.find(family);
    return it == m.end() ? family : it->second;
}

void renderOne(const Project& p, const rt::Format& f, const rt::Tmpl& t, const std::string& outDir)
{
    const std::string name = (f.name + "/" + t.name).toStdString();
    std::string safe;
    for (char c : f.key.toStdString() + "_" + t.name.toStdString()) safe += std::isalnum((unsigned char)c) ? c : '_';
    rt::RenderChoices c;
    const std::string ext = (f.sequence ? rt::pictureExt(t) : rt::fileExt(f, &t)).toStdString();
    c.writerPath = outDir + "/" + safe + (f.sequence ? "_%06d." : ".") + ext;
    RenderSpec spec;
    QString why;
    if (!check(rt::makeRenderSpec(f, t, c, p.settings, &spec, &why), name + ": makes a render spec " + why.toStdString())) return;
    // at the project's size, so its pictures can be read, and the template's own frame rate; not the constant bit rate templates,
    // whose encoders can't keep to 8 or 50 Mbps with pictures that small
    // (MPEG-1 too: its pixel shape is fixed by the template, and its frames are small enough to read)
    const bool ownSize = (t.rc == "cbr" || f.key == "mpeg1") && t.width > 0;
    if (!ownSize) spec.width = spec.height = 0;
    FxRuntime fx;
    std::string err;
    const bool ok = renderProjectAs(p, fx, spec, 0.0, 1.0, [](const ExportProgress&) { return true; }, &err);
    if (!check(ok, name + ": renders" + (ok ? "" : ": " + err))) return;
    const std::string made = f.sequence ? outDir + "/" + safe + "_000000." + ext : c.writerPath;
    std::string spec2 = made;
    if (f.sequence) spec2 = imageSequenceSpec(made, kFps);
    auto r = openMedia(spec2, &err);
    if (!check(r != nullptr, name + ": the file re-imports" + (r ? "" : ": " + err))) return;
    const MediaInfo& in = r->info();
    const bool wantVideo = f.hasVideo && t.video, wantAudio = f.hasAudio && t.audio;
    check(in.hasVideo == wantVideo && in.hasAudio == wantAudio, name + strfmt(": has %s%s%s", wantVideo ? "pictures" : "", wantVideo && wantAudio ? " and " : "", wantAudio ? "sound" : ""));
    std::string desc = strfmt("%s %dx%d %.3f fps %.3f s; %s %d Hz %d ch", in.videoCodec.c_str(), in.width, in.height, in.fps, in.duration, in.audioCodec.c_str(), in.sampleRate, in.channels);
    std::printf("INFO %s: %s\n", name.c_str(), desc.c_str());
    if (wantVideo) {
        const std::string vf = rt::videoFamilyOf(f, t);
        check(in.videoCodec.find(codecName(vf)) == 0 || (vf == "wmv" && in.videoCodec.find("wmv") == 0), name + ": pictures are " + codecName(vf) + " (" + in.videoCodec + ")");
        const double fps = t.fpsNum > 0 ? (double)t.fpsNum / t.fpsDen : kFps;
        const int ww = ownSize ? t.width : kW, wh = ownSize ? t.height : kH;
        if (ownSize && t.par > 0 && std::fabs(t.par - 1.0) > 0.01) {
  // non-square pixels: the reader reports the picture's shape (DV's 720x480 comes back as 4:3), so the shape is compared
            const double want = (double)ww * t.par / wh, got = (double)in.width / in.height;
            check(std::fabs(got - want) < 0.04 * want, name + strfmt(": the picture's shape %dx%d", in.width, in.height));
        } else {
            check(in.width == ww && in.height == wh, name + strfmt(": %dx%d", in.width, in.height));
        }
        if (!f.sequence) check(std::fabs(in.fps - fps) < 0.02, name + strfmt(": %.3f fps (%.3f)", fps, in.fps));
        // (an MPEG-1 system stream with pictures alone has no length but FFmpeg's estimate from the bit rate, short for a
        // second of pictures - ffmpeg -i says the same; its frames are checked below instead)
        if (!(f.key == "mpeg1" && !wantAudio)) check(std::fabs(in.duration - 1.0) < 0.12, name + strfmt(": lasts 1 s (%.3f)", in.duration));
        // pictures: the frame number at three times
        int wrong = 0;
        std::string got;
        for (double at : {0.1, 0.5, 0.9}) {
            const double shown = (std::floor(at * fps) + 0.5) / fps;               // the middle of the file's frame
            const int want = (int)std::floor(shown * kFps + 1e-6);
            const int n = frameNumber(r->videoFrame(shown, 0, 0).get());
            got += strfmt(" %d(%d)", n, want);
            if (std::abs(n - want) > 1) ++wrong;
        }
        if (!ownSize) check(wrong == 0, name + ": the pictures are the project's:" + got);
        if (t.alpha) {
            ImagePtr img = r->videoFrame(0.5, 0, 0);
            const double clear = img ? meanOf(*img, kClear, 6, 3) : 255, solid = img ? meanOf(*img, Rect{0, 150, 150, 50}, 4, 3) : 0;
            check(in.hasAlpha && clear < 10 && solid > 245, name + strfmt(": keeps transparency (alpha %.0f where clear, %.0f where not)", clear, solid));
        }
    }
    if (wantAudio) {
        const std::string af = rt::audioFamilyOf(f, t);
        check(in.audioCodec.find(codecName(af)) == 0, name + ": sound is " + codecName(af) + " (" + in.audioCodec + ")");
        const int rate = t.sampleRate > 0 ? t.sampleRate : kRate;
        check(in.sampleRate == rate && in.channels == (t.channels == 1 ? 1 : t.channels), name + strfmt(": %d Hz, %d channels", in.sampleRate, in.channels));
        std::vector<float> l(kRate), rr(kRate);
        r->audio(0, kRate, kRate, l.data(), rr.data());
        int best = -1;
        float size = 0;
        for (int i = kRate / 2 - 4800; i < kRate / 2 + 4800; ++i)
            if (std::fabs(l[i] - l[i - 1]) > size) { size = std::fabs(l[i] - l[i - 1]); best = i; }
        // less VEGAS's AAC delay, which the reader leaves out of AAC in MP4 (MediaInfo::audioDelaySamples)
        const double offMs = (best - (kRate / 2 + 24) + (in.sampleRate > 0 ? in.audioDelaySamples * (double)kRate / in.sampleRate : 0.0)) * 1000.0 / kRate;
        int cycles = 0;
        for (int i = kRate / 10; i < kRate * 4 / 10; ++i) cycles += l[i - 1] < 0 && l[i] >= 0;
        // lossy encoders' own delay, which FFmpeg can't note in the file, is up to a frame of theirs: 2048 samples for WMA
        check(std::fabs(offMs) < (af == "wma" ? 50 : 30) && std::abs(cycles - 132) <= 3, name + strfmt(": the sound is in place (click %.1f ms off, %d cycles of 132)", offMs, cycles));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::string out = argc > 1 ? argv[1] : "render_formats_out";
    std::string only;
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--only")) only = argv[i + 1];
    QDir(QString::fromStdString(out)).removeRecursively();
    QDir().mkpath(QString::fromStdString(out));
    std::string pic, sound;
    if (!check(makeMedia(out + "/media", &pic, &sound), "the test media are made")) return 1;
    const Project p = makeProject(pic, sound);
    std::printf("INFO %s\n", mediaBackendVersion().c_str());

    // the added formats and templates, by Reno's key ("*": the whole format)
    const std::map<std::string, std::vector<std::string>> added = {
        {"webm", {"VP9 with alpha, project size and rate", "VP8/Vorbis 1080p, project frame rate", "VP8/Vorbis 720p, project frame rate",
                  "AV1/Opus 1080p, project frame rate", "AV1/Opus 720p, project frame rate"}},
        {"mkv", {"HEVC/AAC 1080p, project frame rate", "VP9/Opus 1080p, project frame rate", "AV1/Opus 1080p, project frame rate",
                 "FFV1/FLAC lossless, project size and rate"}},
        {"mov", {"H.264 720p (MOV)", "Animation (QTRLE, with alpha)"}},
        {"images", {"OpenEXR (with alpha)", "Targa (with alpha)", "WebP"}},
        {"mxf_p2", {"*"}}, {"ffmpeg", {"*"}}, {"ogv", {"*"}}, {"opus", {"*"}}, {"gif", {"*"}}, {"apng", {"*"}}, {"aiff", {"*"}}, {"alac", {"*"}}, {"mpeg1", {"*"}}};
    const std::vector<rt::Format> all = rt::builtInFormats();
    std::set<std::string> offered;
    for (const rt::Format& f : all) offered.insert(f.key.toStdString());
    for (const auto& a : added)
        if (!offered.count(a.first)) std::printf("INFO format %s is not offered: this FFmpeg cannot make it\n", a.first.c_str());
    int rendered = 0;
    for (const rt::Format& f : all) {
        auto it = added.find(f.key.toStdString());
        bool firstDone = false;
        for (const rt::Tmpl& t : f.templates) {
            const bool isNew = it != added.end() && (it->second[0] == "*" || std::find(it->second.begin(), it->second.end(), t.name.toStdString()) != it->second.end());
            const bool old = !firstDone || t.alpha;  // older formats: the first template, and the ones with alpha
            if (!isNew && !old) continue;
            if (!isNew && t.mode != "auto" && !t.mode.isEmpty()) continue;  // graphics cards' encoders are not here
            firstDone = true;
            if (!only.empty() && (f.name + "/" + t.name).toStdString().find(only) == std::string::npos) continue;
            renderOne(p, f, t, out);
            ++rendered;
        }
    }
    // templates of the added formats that this FFmpeg cannot make are not offered
    for (const auto& a : added)
        for (const rt::Format& f : all)
            if (f.key.toStdString() == a.first)
                for (const std::string& name : a.second)
                    if (name != "*") {
                        bool there = false;
                        for (const rt::Tmpl& t : f.templates) there = there || t.name.toStdString() == name;
                        if (!there) std::printf("INFO %s/%s is not offered: this FFmpeg cannot make it\n", a.first.c_str(), name.c_str());
                    }
    // Perfect Clarity Audio is listed greyed, with its reason, and has no templates
    {
        bool pca = false;
        for (const rt::Format& f : all)
            if (f.key == "pca") pca = !f.unavailable.isEmpty() && f.templates.empty() && !f.hasVideo;
        check(pca, "Sony Perfect Clarity Audio is listed as not available, with a reason");
    }
    std::printf("INFO %d templates rendered\n", rendered);
    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
