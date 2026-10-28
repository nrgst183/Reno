// File > Render As's formats and templates: see rendertemplates.h. Shared with reno_cli so it renders a template
// exactly as the dialog does.
#include "rendertemplates.h"
#include "strconv.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <algorithm>

namespace reno {
namespace app {
namespace rt {


std::function<void(bool busy)>& hardwareQueryHook()
{
    static std::function<void(bool)> hook;
    return hook;
}

QString thousands(int v)
{
    QString t = QString::number(v);
    for (int i = t.size() - 3; i > 0; i -= 3) t.insert(i, ',');
    return t;
}

QString rateText(int kbps)
{
    if (kbps >= 1000 && kbps % 1000 == 0) return QString("%1 Mbps").arg(kbps / 1000);
    if (kbps >= 1000) return QString("%1 Mbps").arg(kbps / 1000.0, 0, 'f', 1);
    return QString("%1 Kbps").arg(kbps);
}

const HardwareInfo& hardware()
{
    static HardwareInfo info;
    static bool asked = false;
    if (!asked) {
        asked = true;
        if (hardwareQueryHook()) hardwareQueryHook()(true);
        for (const char* fam : {"h264", "hevc"}) info.modes[fam] = videoEncoderModes(fam, true);
        if (hardwareQueryHook()) hardwareQueryHook()(false);
    }
    return info;
}

const char* modeLabel(const QString& mode)
{
    if (mode == "nvenc") return "NVENC";
    if (mode == "qsv") return "Intel QSV";
    if (mode == "amf") return "AMD VCE";
    if (mode == "mf") return "Media Foundation";
    return "";
}

static Tmpl make(const QString& name, int w, int h, int num, int den, int kbps, int maxKbps, const QString& desc = QString())
{
    Tmpl t;
    t.name = name;
    t.description = desc;
    t.width = w;
    t.height = h;
    t.fpsNum = num;
    t.fpsDen = den;
    t.kbps = kbps;
    t.maxKbps = maxKbps;
    return t;
}

static Tmpl audioOnly(const QString& name, int rate, int kbps, int bits, int channels, const QString& desc = QString())
{
    Tmpl t;
    t.name = name;
    t.description = desc;
    t.video = false;
    t.sampleRate = rate;
    t.audioKbps = kbps;
    t.bits = bits;
    t.channels = channels;
    return t;
}

// VEGAS 18's internet templates for AVC and HEVC: each size and rate, then the graphics cards' versions of the main
// ones for the encoders that work here.
static void internetTemplates(Format& f, bool hevc)
{
    const QString desc = QString("Use this setting to create an MP4 (%1/AAC) file for progressive internet downloads.").arg(hevc ? "HEVC" : "AVC");
    struct Row { const char* name; int w, h, num, den, kbps, maxKbps; const char* field; };
    const double m = hevc ? 0.6 : 1.0;  // HEVC needs about 60% of AVC's rate for the same image
    const Row rows[] = {
        {"Internet HD 1080p 29.97 fps", 1920, 1080, 30000, 1001, 16000, 24000, "none"},
        {"Internet HD 1080p 25 fps", 1920, 1080, 25, 1, 16000, 24000, "none"},
        {"Internet HD 1080p 59.94 fps", 1920, 1080, 60000, 1001, 24000, 36000, "none"},
        {"Internet HD 1080p 50 fps", 1920, 1080, 50, 1, 24000, 36000, "none"},
        {"Internet HD 1080p 60i", 1920, 1080, 30000, 1001, 16000, 24000, "upper"},
        {"Internet HD 1080p 50i", 1920, 1080, 25, 1, 16000, 24000, "upper"},
        {"Internet HD 720p 29.97 fps", 1280, 720, 30000, 1001, 8000, 12000, "none"},
        {"Internet HD 720p 25 fps", 1280, 720, 25, 1, 8000, 12000, "none"},
        {"Internet UHD 2160p 29.97 fps", 3840, 2160, 30000, 1001, 40000, 60000, "none"},
        {"Internet UHD 2160p 25 fps", 3840, 2160, 25, 1, 40000, 60000, "none"},
        {"Internet UHD 2160p 59.94 fps", 3840, 2160, 60000, 1001, 60000, 90000, "none"},
        {"Internet UHD 2160p 50 fps", 3840, 2160, 50, 1, 60000, 90000, "none"},
        {"Internet 480p 29.97 fps widescreen", 854, 480, 30000, 1001, 2500, 5000, "none"},
        {"Internet 480p 25 fps widescreen", 854, 480, 25, 1, 2500, 5000, "none"},
        {"Internet 360p 29.97 fps widescreen", 640, 360, 30000, 1001, 1500, 3000, "none"},
        {"Internet 360p 25 fps widescreen", 640, 360, 25, 1, 1500, 3000, "none"},
        {"Internet 480p 29.97 fps 4:3", 640, 480, 30000, 1001, 2500, 5000, "none"},
        {"Internet 360p 29.97 fps 4:3", 480, 360, 30000, 1001, 1500, 3000, "none"},
        {"Internet 720p 29.97 fps portrait", 720, 1280, 30000, 1001, 8000, 12000, "none"},
        {"Internet 480p 29.97 fps portrait", 480, 854, 30000, 1001, 2500, 5000, "none"},
        {"Internet 360p 29.97 fps portrait", 360, 640, 30000, 1001, 1500, 3000, "none"},
    };
    for (const Row& r : rows) {
        if (hevc && (QString(r.name).contains("480p") || QString(r.name).contains("360p") || QString(r.name).endsWith("i"))) continue;
        Tmpl t = make(r.name, r.w, r.h, r.num, r.den, (int)(r.kbps * m), (int)(r.maxKbps * m), desc);
        t.field = r.field;
        t.profile = hevc ? "main" : "high";
        f.templates.push_back(t);
    }
    const std::string fam = hevc ? "hevc" : "h264";
    for (const char* mode : {"nvenc", "qsv", "amf"}) {
        if (!hardware().works(fam, mode)) continue;
        for (const Row& r : rows) {
            const QString n(r.name);
            if (!(n.contains("1080p") || n.contains("2160p") || n.contains("720p")) || n.contains("portrait")) continue;
            if (hevc && n.endsWith("i")) continue;
            Tmpl t = make(n + QString(" (%1)").arg(modeLabel(mode)), r.w, r.h, r.num, r.den, (int)(r.kbps * m), (int)(r.maxKbps * m), desc);
            t.field = r.field;
            t.profile = hevc ? "main" : "high";
            t.mode = mode;
            f.templates.push_back(t);
        }
    }
}

std::vector<Format> builtInFormats()
{
    std::vector<Format> all;
    {
        Format f;
        f.key = "avc";
        f.name = "MAGIX AVC/AAC MP4";
        f.ext = "mp4";
        f.blurb = "H.264 pictures and AAC sound in an MP4 file: what the internet and most players take.";
        f.videoFamily = "h264";
        f.audioFamily = "aac";
        f.profiles = QStringList() << "baseline|Baseline" << "main|Main" << "high|High";
        internetTemplates(f, false);
        Tmpl bd = make("Blu-ray 1920x1080-24p, 25 Mbps video stream", 1920, 1080, 24000, 1001, 25000, 40000);
        bd.profile = "high";
        bd.level = "4.1";
        f.templates.push_back(bd);
        Tmpl ipad = make("Apple iPad/iPhone 4 720p30 Video", 1280, 720, 30000, 1001, 5000, 8000,
                         "Use this setting to create an MP4 file that you can sync to and play on Apple iPad, iPhone 4 and iPod Touch 4th generation.");
        ipad.profile = "main";
        ipad.level = "3.1";
        ipad.audioKbps = 128;
        ipad.sampleRate = 44100;
        f.templates.push_back(ipad);
        for (const char* rate : {"29.97", "25"}) {
            const QString base = QString("Internet HD 1080p %1 fps").arg(rate);
            for (const Tmpl& t0 : std::vector<Tmpl>(f.templates))
                if (t0.name == base) {
                    Tmpl t = t0;
                    t.name = base + ", 5.1 Surround";
                    t.description = "Use this setting to create an MP4 (AVC/AAC) file with 5.1 surround sound (448 Kbps AAC, L R C LFE Ls Rs).";
                    t.channels = 6;
                    t.audioKbps = 448;
                    f.templates.push_back(t);
                }
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "hevc";
        f.name = "MAGIX HEVC/AAC MP4";
        f.ext = "mp4";
        f.blurb = "HEVC (H.265) pictures and AAC sound in an MP4 file: smaller files than AVC for the same picture.";
        f.videoFamily = "hevc";
        f.audioFamily = "aac";
        f.profiles = QStringList() << "main|Main" << "main10|Main 10";
        internetTemplates(f, true);
        all.push_back(f);
    }
    {
        Format f;
        f.key = "wmv";
        f.name = "Windows Media Video V11";
        f.ext = "wmv";
        f.blurb = "Windows Media pictures and sound (an ASF file).";
        f.videoFamily = "wmv";
        f.audioFamily = "wma";
        f.templates.push_back(make("8 Mbps HD 1080-30p Video", 1920, 1080, 30000, 1001, 8000, 0, "Use this setting for 1080p HD content."));
        f.templates.push_back(make("6.4 Mbps HD 720-30p Video", 1280, 720, 30000, 1001, 6400, 0, "Use this setting for 720p HD content."));
        f.templates.push_back(make("3 Mbps Video", 640, 480, 30000, 1001, 3000, 0, "Use this setting for high quality standard definition video."));
        f.templates.push_back(make("512 Kbps Video", 320, 240, 30000, 1001, 512, 0, "Use this setting for video to be streamed over broadband."));
        for (Tmpl& t : f.templates) {
            t.rc = "cbr";
            t.audioKbps = 128;
            t.sampleRate = 44100;
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "avi";
        f.name = "Video for Windows";
        f.ext = "avi";
        f.blurb = "AVI files: uncompressed, DV, or lossless YUV (UT Video, in place of Sony YUV).";
        f.videoFamily = "rawvideo";
        f.audioFamily = "pcm";
        f.bitRate = false;
        f.audioRate = false;
        f.alphaPossible = true;
        Tmpl d = make("Default Template (uncompressed)", 0, 0, 0, 1, 0, 0, "Audio and video uncompressed, at the project's size and rate.");
        d.par = 0;
        d.field = "";
        d.sampleRate = 0;
        f.templates.push_back(d);
        struct DvRow { const char* name; int h, num, den; double par; };
        const DvRow dv[] = {{"NTSC DV", 480, 30000, 1001, 0.9091}, {"NTSC DV Widescreen", 480, 30000, 1001, 1.2121},
                            {"PAL DV", 576, 25, 1, 1.0926}, {"PAL DV Widescreen", 576, 25, 1, 1.4568}};
        for (const DvRow& r : dv) {
            Tmpl t = make(r.name, 720, r.h, r.num, r.den, 25000, 0, QString("Audio: 48,000 Hz, 16 Bit, Stereo, PCM. Video: %1 fps, 720x%2, Lower field first, DV.")
                                                                         .arg((double)r.num / r.den, 0, 'f', 3).arg(r.h));
            t.field = "lower";
            t.par = r.par;
            t.picture = "dv";
            f.templates.push_back(t);
        }
        for (const char* n : {"HD 1080-30p YUV", "HD 1080-60i YUV", "HD 720-30p YUV"}) {
            const QString s(n);
            Tmpl t = make(s, s.contains("720") ? 1280 : 1920, s.contains("720") ? 720 : 1080, 30000, 1001, 0, 0, "Lossless YUV pictures (UT Video) and uncompressed sound.");
            if (s.contains("60i")) t.field = "upper";
            t.picture = "utvideo";
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "mov";
        f.name = "QuickTime 7";
        f.ext = "mov";
        f.blurb = "QuickTime files: Apple ProRes, PNG or Animation (both keep transparency), or H.264.";
        f.videoFamily = "prores";
        f.audioFamily = "pcm";
        f.bitRate = false;
        f.audioRate = false;
        f.alphaPossible = true;
        f.profiles = QStringList() << "proxy|ProRes 422 Proxy" << "lt|ProRes 422 LT" << "standard|ProRes 422" << "hq|ProRes 422 HQ" << "4444|ProRes 4444";
        struct Row { const char* name; const char* profile; };
        const Row rows[] = {{"Apple ProRes 422 HQ", "hq"}, {"Apple ProRes 422", "standard"}, {"Apple ProRes 422 LT", "lt"},
                            {"Apple ProRes 422 Proxy", "proxy"}, {"Apple ProRes 4444 (with alpha)", "4444"}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, 0, 0, 0, 1, 0, 0, "At the project's size and rate, with uncompressed sound.");
            t.profile = r.profile;
            t.alpha = QString(r.profile) == "4444";
            t.sampleRate = 0;
            t.par = 0;
            t.field = "";
            f.templates.push_back(t);
        }
        Tmpl png = make("PNG (with alpha)", 0, 0, 0, 1, 0, 0, "Lossless pictures that keep transparency, at the project's size and rate.");
        png.picture = "png";
        png.alpha = true;
        png.sampleRate = 0;
        png.par = 0;
        png.field = "";
        f.templates.push_back(png);
        Tmpl h264 = make("H.264 1080p (MOV)", 1920, 1080, 0, 1, 16000, 24000, "H.264 pictures and AAC sound in a QuickTime file.");
        h264.picture = "h264";
        h264.profile = "high";
        f.templates.push_back(h264);
        Tmpl h720 = make("H.264 720p (MOV)", 1280, 720, 0, 1, 8000, 12000, "H.264 pictures and AAC sound in a QuickTime file.");
        h720.picture = "h264";
        h720.profile = "high";
        f.templates.push_back(h720);
        Tmpl rle = make("Animation (QTRLE, with alpha)", 0, 0, 0, 1, 0, 0, "QuickTime's lossless Animation pictures, which keep transparency, at the project's size and rate.");
        rle.picture = "qtrle";
        rle.alpha = true;
        rle.sampleRate = 0;
        f.templates.push_back(rle);
        all.push_back(f);
    }
    {
        Format f;
        f.key = "mxf";
        f.name = "Sony MXF";
        f.ext = "mxf";
        f.blurb = "MXF files with MPEG-2 HD422 pictures (XDCAM HD422) and uncompressed sound, for broadcast.";
        f.videoFamily = "mpeg2";
        f.audioFamily = "pcm";
        f.audioRate = false;
        struct Row { const char* name; int w, h, num, den, kbps; const char* field; };
        const Row rows[] = {{"HD422 1920x1080-60i, 50 Mbps", 1920, 1080, 30000, 1001, 50000, "upper"},
                            {"HD422 1920x1080-50i, 50 Mbps", 1920, 1080, 25, 1, 50000, "upper"},
                            {"HD422 1920x1080-30p, 50 Mbps", 1920, 1080, 30000, 1001, 50000, "none"},
                            {"HD422 1920x1080-25p, 50 Mbps", 1920, 1080, 25, 1, 50000, "none"},
                            {"HD422 1920x1080-24p, 50 Mbps", 1920, 1080, 24000, 1001, 50000, "none"},
                            {"HD422 1280x720-60p, 50 Mbps", 1280, 720, 60000, 1001, 50000, "none"},
                            {"HD422 1280x720-50p, 50 Mbps", 1280, 720, 50, 1, 50000, "none"}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, r.w, r.h, r.num, r.den, r.kbps, r.kbps, "Audio: 48,000 Hz, 24 Bit, PCM. Video: MPEG-2 4:2:2.");
            t.rc = "cbr";
            t.field = r.field;
            t.bits = 24;
            t.keySeconds = 0.5;
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        // VEGAS's MainConcept MPEG-2 (manual: Rendering MPEG Files, Rendering Projects for Use in DVD Architect):
        // program streams for DVD authoring. The "Video Stream" templates make the frame alone (sound is rendered
        // apart, as AC-3 or PCM); NTSC/PAL DVD make both.
        Format f;
        f.key = "mpeg2ps";
        f.name = "MainConcept MPEG-2";
        f.ext = "mpg";
        f.blurb = "MPEG-2 program streams for DVD authoring (DVD Architect and others). Frame widths must be divisible by 16 and heights by 2.";
        f.videoFamily = "mpeg2";
        f.audioFamily = "ac3";
        struct Row { const char* name; int w, h, num, den, kbps, maxKbps; const char* field; double par; bool audio; const char* desc; };
        const Row rows[] = {
            {"DVD Architect NTSC Video Stream", 720, 480, 30000, 1001, 8000, 9800, "lower", 0.9091, false, "Video: 720x480, 29.97 fps, 4:3, up to 9.8 Mbps. Render the sound apart as AC-3 or PCM."},
            {"DVD Architect NTSC Widescreen Video Stream", 720, 480, 30000, 1001, 8000, 9800, "lower", 1.2121, false, "Video: 720x480, 29.97 fps, 16:9, up to 9.8 Mbps."},
            {"DVD Architect 24p NTSC Video Stream", 720, 480, 24000, 1001, 8000, 9800, "none", 0.9091, false, "Video: 720x480, 23.976 fps progressive, 4:3, with 2-3 pulldown flags in the DVD."},
            {"DVD Architect 24p NTSC Widescreen Video Stream", 720, 480, 24000, 1001, 8000, 9800, "none", 1.2121, false, "Video: 720x480, 23.976 fps progressive, 16:9."},
            {"DVD Architect PAL Video Stream", 720, 576, 25, 1, 8000, 9800, "upper", 1.0667, false, "Video: 720x576, 25 fps, 4:3, up to 9.8 Mbps."},
            {"DVD Architect PAL Widescreen Video Stream", 720, 576, 25, 1, 8000, 9800, "upper", 1.4222, false, "Video: 720x576, 25 fps, 16:9, up to 9.8 Mbps."},
            {"NTSC DVD", 720, 480, 30000, 1001, 7000, 9800, "lower", 0.9091, true, "Video: 720x480, 29.97 fps, 4:3. Audio: AC-3, 192 Kbps, 48,000 Hz, Stereo."},
            {"NTSC DVD Widescreen", 720, 480, 30000, 1001, 7000, 9800, "lower", 1.2121, true, "Video: 720x480, 29.97 fps, 16:9. Audio: AC-3, 192 Kbps, 48,000 Hz, Stereo."},
            {"PAL DVD", 720, 576, 25, 1, 7000, 9800, "upper", 1.0667, true, "Video: 720x576, 25 fps, 4:3. Audio: AC-3, 192 Kbps, 48,000 Hz, Stereo."},
            {"PAL DVD Widescreen", 720, 576, 25, 1, 7000, 9800, "upper", 1.4222, true, "Video: 720x576, 25 fps, 16:9. Audio: AC-3, 192 Kbps, 48,000 Hz, Stereo."},
            {"HD 1080-30p, 15 Mbps", 1920, 1080, 30000, 1001, 15000, 20000, "none", 1.0, true, "Video: 1920x1080, 29.97 fps progressive. Audio: AC-3, 192 Kbps, 48,000 Hz, Stereo."},
            {"HD 720-60p, 15 Mbps", 1280, 720, 60000, 1001, 15000, 20000, "none", 1.0, true, "Video: 1280x720, 59.94 fps progressive. Audio: AC-3, 192 Kbps, 48,000 Hz, Stereo."}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, r.w, r.h, r.num, r.den, r.kbps, r.maxKbps, r.desc);
            t.field = r.field;
            t.par = r.par;
            t.audio = r.audio;
            t.sampleRate = 48000;
            t.audioKbps = 192;
            t.rc = "vbr";
            t.keySeconds = r.num == 25 ? 0.6 : 0.5;
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        // MPEG-1 (VEGAS Pro 23 renders it): the MPEG-1 system stream (.mpg) of mpeg1video pictures and MPEG-1 Layer II
        // sound, which FFmpeg's "mpeg" muxer writes as MPEG-1 (not MPEG-2) program stream when the pictures are MPEG-1.
        // MPEG-1 has no interlaced coding, so every template is progressive.
        Format f;
        f.key = "mpeg1";
        f.name = "MPEG-1";
        f.ext = "mpg";
        f.blurb = "MPEG-1 system streams: Video CD and plain MPEG-1 files, progressive pictures and Layer II sound.";
        f.videoFamily = "mpeg1";
        f.audioFamily = "mp2";
        struct Row { const char* name; int w, h, num, den, kbps, maxKbps; double par; bool audio; bool cbr; int rate; double key; const char* desc; };
        const Row rows[] = {
            {"Video CD NTSC", 352, 240, 30000, 1001, 1150, 1150, 0.9091, true, true, 44100, 0.5, "Video: 352x240, 29.97 fps, 1.15 Mbps constant. Audio: MPEG-1 Layer II, 224 Kbps, 44,100 Hz, Stereo."},
            {"Video CD PAL", 352, 288, 25, 1, 1150, 1150, 1.0909, true, true, 44100, 0.48, "Video: 352x288, 25 fps, 1.15 Mbps constant. Audio: MPEG-1 Layer II, 224 Kbps, 44,100 Hz, Stereo."},
            {"MPEG-1 NTSC Video Stream", 352, 240, 30000, 1001, 1500, 1856, 0.9091, false, false, 44100, 0.5, "Video: 352x240, 29.97 fps, up to 1.856 Mbps. Render the sound apart."},
            {"MPEG-1 PAL Video Stream", 352, 288, 25, 1, 1500, 1856, 1.0909, false, false, 44100, 0.48, "Video: 352x288, 25 fps, up to 1.856 Mbps. Render the sound apart."},
            {"MPEG-1 NTSC", 352, 240, 30000, 1001, 1500, 1856, 0.9091, true, false, 44100, 0.5, "Video: 352x240, 29.97 fps. Audio: MPEG-1 Layer II, 224 Kbps, 44,100 Hz, Stereo."},
            {"MPEG-1 PAL", 352, 288, 25, 1, 1500, 1856, 1.0909, true, false, 44100, 0.48, "Video: 352x288, 25 fps. Audio: MPEG-1 Layer II, 224 Kbps, 44,100 Hz, Stereo."},
            {"MPEG-1 640x480 29.97 fps", 640, 480, 30000, 1001, 3000, 4000, 1.0, true, false, 48000, 0.5, "Video: 640x480, 29.97 fps. Audio: MPEG-1 Layer II, 224 Kbps, 48,000 Hz, Stereo."},
            {"MPEG-1 720x576 25 fps", 720, 576, 25, 1, 4000, 5000, 1.0667, true, false, 48000, 0.48, "Video: 720x576, 25 fps. Audio: MPEG-1 Layer II, 224 Kbps, 48,000 Hz, Stereo."}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, r.w, r.h, r.num, r.den, r.kbps, r.maxKbps, r.desc);
            t.field = "none";
            t.par = r.par;
            t.audio = r.audio;
            t.sampleRate = r.rate;
            t.audioKbps = 224;
            t.rc = r.cbr ? "cbr" : "vbr";
            t.keySeconds = r.key;
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "xavcs";
        f.name = "Sony XAVC S";
        f.ext = "mp4";
        f.blurb = "XAVC S: H.264 long-GOP pictures and uncompressed sound in an MP4 file, as Sony cameras record.";
        f.videoFamily = "h264";
        f.audioFamily = "pcm";
        f.profiles = QStringList() << "main|Main" << "high|High";
        struct Row { const char* name; int w, h, num, den, kbps; };
        const Row rows[] = {{"XAVC S HD 1920x1080-60p, 50 Mbps", 1920, 1080, 60000, 1001, 50000}, {"XAVC S HD 1920x1080-50p, 50 Mbps", 1920, 1080, 50, 1, 50000},
                            {"XAVC S HD 1920x1080-30p, 50 Mbps", 1920, 1080, 30000, 1001, 50000}, {"XAVC S HD 1920x1080-25p, 50 Mbps", 1920, 1080, 25, 1, 50000},
                            {"XAVC S HD 1920x1080-24p, 50 Mbps", 1920, 1080, 24000, 1001, 50000}, {"XAVC S 4K 3840x2160-30p, 100 Mbps", 3840, 2160, 30000, 1001, 100000},
                            {"XAVC S 4K 3840x2160-25p, 100 Mbps", 3840, 2160, 25, 1, 100000}, {"XAVC S 4K 3840x2160-24p, 100 Mbps", 3840, 2160, 24000, 1001, 100000}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, r.w, r.h, r.num, r.den, r.kbps, r.kbps, "Audio: 48,000 Hz, 16 Bit, Stereo, PCM. Video: AVC long GOP.");
            t.profile = "high";
            t.level = r.h > 1080 ? "5.1" : "4.2";
            t.keySeconds = 0.5;
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "wav";
        f.name = "Wave (Microsoft)";
        f.ext = "wav";
        f.blurb = "Uncompressed sound in a WAV file.";
        f.hasVideo = false;
        f.audioFamily = "pcm";
        f.audioRate = false;
        f.templates.push_back(audioOnly("Default Template", 0, 0, 16, 2, "Audio: the project's sample rate, 16 Bit, Stereo, PCM."));
        f.templates.push_back(audioOnly("44,100 Hz, 16 Bit, Stereo, PCM", 44100, 0, 16, 2));
        f.templates.push_back(audioOnly("48,000 Hz, 16 Bit, Stereo, PCM", 48000, 0, 16, 2));
        f.templates.push_back(audioOnly("48,000 Hz, 24 Bit, Stereo, PCM", 48000, 0, 24, 2));
        f.templates.push_back(audioOnly("48,000 Hz, 32 Bit (float), Stereo, IEEE", 48000, 0, 32, 2));
        f.templates.push_back(audioOnly("44,100 Hz, 16 Bit, Mono, PCM", 44100, 0, 16, 1));
        {
            Tmpl mm = audioOnly("48,000 Hz, 24 Bit, Multiple Mono, PCM", 48000, 0, 24, 1,
                                "Audio: 48,000 Hz, 24 Bit, PCM: each of the project's channels in a mono file of its own (Left, Right, Center, LFE, Left Surround, Right Surround).");
            mm.multipleMono = true;
            f.templates.push_back(mm);
        }
        all.push_back(f);
        Format w64 = f;
        w64.key = "w64";
        w64.name = "Sony Wave64";
        w64.ext = "w64";
        w64.blurb = "Uncompressed sound in a Wave64 file (no 4 GB limit).";
        all.push_back(w64);
    }
    {
        Format f;
        f.key = "mp3";
        f.name = "MP3 Audio";
        f.ext = "mp3";
        f.blurb = "MPEG Layer-3 sound.";
        f.hasVideo = false;
        f.audioFamily = "mp3";
        struct Row { const char* name; int kbps; const char* desc; };
        const Row rows[] = {{"320 Kbps, CD Transparent Audio", 320, "One minute of audio compresses to a file size of approximately 2.29 MB (about 4:1 compression)."},
                            {"256 Kbps, CD Transparent Audio", 256, "One minute of audio compresses to a file size of approximately 1.83 MB (about 5:1 compression)."},
                            {"192 Kbps, CD Transparent Audio", 192, "One minute of audio compresses to a file size of approximately 1.37 MB (about 7:1 compression)."},
                            {"160 Kbps, CD Transparent Audio", 160, "One minute of audio compresses to a file size of approximately 1.14 MB (about 9:1 compression)."},
                            {"128 Kbps, CD Quality Audio", 128, "One minute of audio compresses to a file size of approximately 0.91 MB (about 11:1 compression)."},
                            {"96 Kbps, Near CD Quality Audio", 96, "One minute of audio compresses to a file size of approximately 0.68 MB (about 14:1 compression)."},
                            {"64 Kbps, FM Radio Quality Audio", 64, "One minute of audio compresses to a file size of approximately 0.45 MB (about 22:1 compression)."}};
        for (const Row& r : rows) f.templates.push_back(audioOnly(r.name, 44100, r.kbps, 16, 2, r.desc));
        all.push_back(f);
    }
    {
        Format f;
        f.key = "flac";
        f.name = "FLAC Audio";
        f.ext = "flac";
        f.blurb = "Losslessly compressed sound.";
        f.hasVideo = false;
        f.audioFamily = "flac";
        f.audioRate = false;
        f.templates.push_back(audioOnly("Default Template", 0, 0, 16, 2, "Audio: the project's sample rate, 16 Bit, Stereo, FLAC."));
        f.templates.push_back(audioOnly("44,100 Hz, 16 Bit, Stereo, FLAC", 44100, 0, 16, 2));
        f.templates.push_back(audioOnly("48,000 Hz, 24 Bit, Stereo, FLAC", 48000, 0, 24, 2));
        all.push_back(f);
    }
    {
        Format f;
        f.key = "ogg";
        f.name = "OggVorbis";
        f.ext = "ogg";
        f.blurb = "Ogg Vorbis sound.";
        f.hasVideo = false;
        f.audioFamily = "vorbis";
        f.templates.push_back(audioOnly("128 Kbps, CD Quality", 44100, 128, 16, 2, "Audio: 128 Kbps average bit rate. Actual rate varies across entire song depending on source content."));
        f.templates.push_back(audioOnly("96 Kbps, Near-CD Quality", 44100, 96, 16, 2, "Audio: 96 Kbps average bit rate."));
        f.templates.push_back(audioOnly("350 Kbps, CD Quality", 44100, 350, 16, 2, "Audio: 350 Kbps average bit rate."));
        all.push_back(f);
    }
    {
        Format f;
        f.key = "m4a";
        f.name = "MAGIX AAC Audio";
        f.ext = "m4a";
        f.blurb = "AAC sound in an MP4 audio file.";
        f.hasVideo = false;
        f.audioFamily = "aac";
        f.templates.push_back(audioOnly("320 Kbps Stereo", 48000, 320, 16, 2));
        f.templates.push_back(audioOnly("192 Kbps Stereo", 48000, 192, 16, 2));
        f.templates.push_back(audioOnly("128 Kbps Stereo", 44100, 128, 16, 2));
        f.templates.push_back(audioOnly("448 Kbps 5.1 Surround", 48000, 448, 16, 6, "Audio: 448 Kbps, 48,000 Hz, 5.1 Surround (L R C LFE Ls Rs), AAC."));
        f.templates.push_back(audioOnly("640 Kbps 5.1 Surround", 48000, 640, 16, 6, "Audio: 640 Kbps, 48,000 Hz, 5.1 Surround (L R C LFE Ls Rs), AAC."));
        all.push_back(f);
    }
    {
        // VEGAS's Dolby Digital AC-3 Studio plug-in (ac3studioplug): its templates and descriptions. VEGAS's encoder
        // is Dolby's; Reno uses FFmpeg's AC-3 encoder, which has no Automatic Gain Control (the "AGC on" templates
        // make the same file).
        Format f;
        f.key = "ac3";
        f.name = "Dolby Digital AC-3 Studio";
        f.ext = "ac3";
        f.blurb = "Dolby Digital (AC-3) sound, stereo or 5.1, for DVD and Blu-ray soundtracks.";
        f.hasVideo = false;
        f.audioFamily = "ac3";
        f.templates.push_back(audioOnly("Stereo DVD", 48000, 192, 16, 2,
                                        "Audio: 192 Kbps, 48,000 Hz, Stereo, Automatic Gain Control off.\nUse this setting for stereo DVD soundtracks."));
        f.templates.push_back(audioOnly("5.1 Surround DVD", 48000, 448, 16, 6,
                                        "Audio: 448 Kbps, 48,000 Hz, 5.1 Surround, Automatic Gain Control off.\nUse this setting for 5.1 Surround DVD soundtracks."));
        f.templates.push_back(audioOnly("Stereo DVD, AGC on", 48000, 192, 16, 2,
                                        "Audio: 192 Kbps, 48,000 Hz, Stereo, Automatic Gain Control on.\nUse this setting for stereo DVD soundtracks."));
        f.templates.push_back(audioOnly("5.1 Surround DVD, AGC on", 48000, 448, 16, 6,
                                        "Audio: 448 Kbps, 48,000 Hz, 5.1 Surround, Automatic Gain Control on.\nUse this setting for 5.1 Surround DVD soundtracks."));
        all.push_back(f);
    }
    {
        Format f;
        f.key = "wma";
        f.name = "Windows Media Audio V11";
        f.ext = "wma";
        f.blurb = "Windows Media sound.";
        f.hasVideo = false;
        f.audioFamily = "wma";
        f.templates.push_back(audioOnly("192 Kbps, 44 kHz, stereo CBR", 44100, 192, 16, 2));
        f.templates.push_back(audioOnly("128 Kbps, 44 kHz, stereo CBR", 44100, 128, 16, 2));
        f.templates.push_back(audioOnly("64 Kbps, 44 kHz, stereo CBR", 44100, 64, 16, 2));
        all.push_back(f);
    }
    {
        Format f;
        f.key = "images";
        f.name = "Image Sequence";
        f.ext = "png";
        f.blurb = "One picture file per frame, numbered: name_000000.png, name_000001.png ...";
        f.hasAudio = false;
        f.bitRate = false;
        f.alphaPossible = true;
        f.sequence = true;
        struct Row { const char* name; const char* picture; bool alpha; };
        const Row rows[] = {{"PNG (with alpha)", "png", true}, {"PNG", "png", false}, {"JPEG", "jpeg", false}, {"TIFF", "tiff", false}, {"BMP", "bmp", false},
                            {"OpenEXR (with alpha)", "exr", true}, {"Targa (with alpha)", "tga", true}, {"WebP", "webp", false}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, 0, 0, 0, 1, 0, 0, QString("Video: the project's size and rate, %1 stills%2.").arg(r.name).arg(r.alpha ? " that keep transparency" : ""));
            t.picture = r.picture;
            t.alpha = r.alpha;
            t.audio = false;
            t.par = 0;
            t.field = "";
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "mkv";
        f.name = "Matroska (MKV)";
        f.ext = "mkv";
        f.blurb = "Not a VEGAS format: H.264 and AAC in a Matroska file.";
        f.videoFamily = "h264";
        f.audioFamily = "aac";
        f.profiles = QStringList() << "baseline|Baseline" << "main|Main" << "high|High";
        Tmpl t = make("HD 1080p, project frame rate", 1920, 1080, 0, 1, 16000, 24000);
        t.profile = "high";
        f.templates.push_back(t);
        Tmpl p = make("Project size and rate", 0, 0, 0, 1, 16000, 24000);
        p.profile = "high";
        p.par = 0;
        p.field = "";
        f.templates.push_back(p);
        Tmpl hevc = make("HEVC/AAC 1080p, project frame rate", 1920, 1080, 0, 1, 10000, 15000, "HEVC (H.265) pictures and AAC sound in a Matroska file.");
        hevc.picture = "hevc";
        hevc.profile = "main";
        f.templates.push_back(hevc);
        Tmpl vp9 = make("VP9/Opus 1080p, project frame rate", 1920, 1080, 0, 1, 8000, 12000, "VP9 pictures and Opus sound in a Matroska file.");
        vp9.picture = "vp9";
        vp9.sound = "opus";
        f.templates.push_back(vp9);
        Tmpl av1 = make("AV1/Opus 1080p, project frame rate", 1920, 1080, 0, 1, 6000, 9000, "AV1 pictures and Opus sound in a Matroska file.");
        av1.picture = "av1";
        av1.sound = "opus";
        f.templates.push_back(av1);
        Tmpl ffv1 = make("FFV1/FLAC lossless, project size and rate", 0, 0, 0, 1, 0, 0, "Lossless pictures (FFV1) and lossless sound (FLAC), for keeping.");
        ffv1.picture = "ffv1";
        ffv1.sound = "flac";
        ffv1.sampleRate = 0;
        f.templates.push_back(ffv1);
        all.push_back(f);
    }
    {
        Format f;
        f.key = "webm";
        f.name = "WebM";
        f.ext = "webm";
        f.blurb = "Not a VEGAS format: VP9, VP8 or AV1 pictures and Opus or Vorbis sound, for web pages.";
        f.videoFamily = "vp9";
        f.audioFamily = "opus";
        f.alphaPossible = true;
        Tmpl t = make("HD 1080p, project frame rate", 1920, 1080, 0, 1, 8000, 12000);
        f.templates.push_back(t);
        Tmpl h = make("HD 720p, project frame rate", 1280, 720, 0, 1, 4000, 6000);
        f.templates.push_back(h);
        Tmpl a = make("VP9 with alpha, project size and rate", 0, 0, 0, 1, 8000, 12000, "VP9 pictures that keep transparency (as browsers show them), and Opus sound.");
        a.alpha = true;
        f.templates.push_back(a);
        for (int k = 0; k < 2; ++k) {
            Tmpl v = make(k ? "VP8/Vorbis 720p, project frame rate" : "VP8/Vorbis 1080p, project frame rate", k ? 1280 : 1920, k ? 720 : 1080, 0, 1,
                          k ? 5000 : 10000, k ? 7500 : 15000, "VP8 pictures and Vorbis sound: WebM as older players take it.");
            v.picture = "vp8";
            v.sound = "vorbis";
            f.templates.push_back(v);
        }
        for (int k = 0; k < 2; ++k) {
            Tmpl v = make(k ? "AV1/Opus 720p, project frame rate" : "AV1/Opus 1080p, project frame rate", k ? 1280 : 1920, k ? 720 : 1080, 0, 1,
                          k ? 3000 : 6000, k ? 4500 : 9000, "AV1 pictures and Opus sound: the smallest files for the picture.");
            v.picture = "av1";
            f.templates.push_back(v);
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "ogv";
        f.name = "Ogg Theora";
        f.ext = "ogv";
        f.blurb = "Not a VEGAS format: Theora pictures and Vorbis sound in an Ogg file.";
        f.videoFamily = "theora";
        f.audioFamily = "vorbis";
        f.templates.push_back(make("HD 1080p, project frame rate", 1920, 1080, 0, 1, 10000, 0));
        f.templates.push_back(make("HD 720p, project frame rate", 1280, 720, 0, 1, 5000, 0));
        f.templates.push_back(make("480p widescreen, project frame rate", 854, 480, 0, 1, 2500, 0));
        for (Tmpl& t : f.templates) t.audioKbps = 160;
        all.push_back(f);
    }
    {
        Format f;
        f.key = "opus";
        f.name = "Ogg Opus";
        f.ext = "opus";
        f.blurb = "Not a VEGAS format: Opus sound, the best sound for its size.";
        f.hasVideo = false;
        f.audioFamily = "opus";
        for (int kbps : {256, 160, 128, 96, 64})
            f.templates.push_back(audioOnly(QString("%1 Kbps Stereo").arg(kbps), 48000, kbps, 16, 2,
                                            QString("Audio: %1 Kbps, 48,000 Hz (Opus is always 48 kHz), Stereo, Opus.").arg(kbps)));
        all.push_back(f);
    }
    {
        Format f;
        f.key = "gif";
        f.name = "Animated GIF";
        f.ext = "gif";
        f.blurb = "Not a VEGAS format: a looping GIF, 256 colours a picture (no sound). GIF counts time in hundredths of a second.";
        f.videoFamily = "gif";
        f.hasAudio = false;
        f.bitRate = false;
        f.alphaPossible = true;
        struct Row { const char* name; int w, h, fps; bool alpha; };
        const Row rows[] = {{"640x360, 25 fps", 640, 360, 25, false}, {"480x270, 20 fps", 480, 270, 20, false}, {"320x180, 10 fps (small)", 320, 180, 10, false},
                            {"Project size, 25 fps", 0, 0, 25, false}, {"Project size, 25 fps, with transparency", 0, 0, 25, true}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, r.w, r.h, r.fps, 1, 0, 0, QString("An animated GIF that loops, %1 frames a second%2.").arg(r.fps).arg(r.alpha ? ", keeping transparency (clear or not, nothing in between)" : ""));
            t.alpha = r.alpha;
            t.audio = false;
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "apng";
        f.name = "Animated PNG";
        f.ext = "apng";
        f.blurb = "Not a VEGAS format: a looping animated PNG, every colour and transparency kept (no sound).";
        f.videoFamily = "apng";
        f.hasAudio = false;
        f.bitRate = false;
        f.alphaPossible = true;
        Tmpl a = make("Project size and rate, with alpha", 0, 0, 0, 1, 0, 0, "Lossless pictures that keep transparency.");
        a.alpha = true;
        a.audio = false;
        f.templates.push_back(a);
        Tmpl o = make("Project size and rate", 0, 0, 0, 1, 0, 0, "Lossless pictures.");
        o.audio = false;
        f.templates.push_back(o);
        all.push_back(f);
    }
    {
        Format f;
        f.key = "aiff";
        f.name = "AIFF";
        f.ext = "aif";
        f.blurb = "Uncompressed sound in an AIFF file (Apple's WAV).";
        f.hasVideo = false;
        f.audioFamily = "pcm";
        f.audioRate = false;
        f.templates.push_back(audioOnly("Default Template", 0, 0, 16, 2, "Audio: the project's sample rate, 16 Bit, Stereo, PCM."));
        f.templates.push_back(audioOnly("44,100 Hz, 16 Bit, Stereo, PCM", 44100, 0, 16, 2));
        f.templates.push_back(audioOnly("48,000 Hz, 24 Bit, Stereo, PCM", 48000, 0, 24, 2));
        {
            Tmpl mm = audioOnly("48,000 Hz, 24 Bit, Multiple Mono, PCM", 48000, 0, 24, 1,
                                "Audio: 48,000 Hz, 24 Bit, PCM: each of the project's channels in a mono file of its own (Left, Right, Center, LFE, Left Surround, Right Surround).");
            mm.multipleMono = true;
            f.templates.push_back(mm);
        }
        all.push_back(f);
    }
    {
        Format f;
        f.key = "alac";
        f.name = "Apple Lossless (ALAC)";
        f.ext = "m4a";
        f.blurb = "Losslessly compressed sound in an MP4 audio file, as iTunes and Apple devices play it.";
        f.hasVideo = false;
        f.audioFamily = "alac";
        f.audioRate = false;
        f.templates.push_back(audioOnly("44,100 Hz, 16 Bit, Stereo, ALAC", 44100, 0, 16, 2));
        f.templates.push_back(audioOnly("48,000 Hz, 24 Bit, Stereo, ALAC", 48000, 0, 24, 2));
        all.push_back(f);
    }
    {
        // Panasonic P2 MXF: FFmpeg's MXF muxer (OP1a) with the P2 codecs: DVCPRO (25, 50, HD: dvvideo) and AVC-Intra 50/100
        // (x264's avcintra-class). VEGAS's plug-in writes the P2 card's folder structure with one OP-Atom file per stream.
        Format f;
        f.key = "mxf_p2";
        f.name = "Panasonic P2 MXF";
        f.ext = "mxf";
        f.blurb = "MXF files (OP1a) with the P2 codecs: DVCPRO25, DVCPRO50, DVCPRO HD and AVC-Intra, and uncompressed sound. Not the P2 card's folder structure.";
        f.videoFamily = "dv";
        f.audioFamily = "pcm";
        f.audioRate = false;
        struct Dv { const char* name; int w, h, num, den; const char* field; double par; const char* profile; int mbps; const char* sizeText; };
        const Dv dv[] = {
            {"DVCPRO25 NTSC", 720, 480, 30000, 1001, "lower", 0.9091, "dv25", 25, "4:1:1"},
            {"DVCPRO25 PAL", 720, 576, 25, 1, "lower", 1.0926, "dv25", 25, "4:2:0"},
            {"DVCPRO50 NTSC", 720, 480, 30000, 1001, "lower", 0.9091, "dv50", 50, "4:2:2"},
            {"DVCPRO50 PAL", 720, 576, 25, 1, "lower", 1.0926, "dv50", 50, "4:2:2"},
            {"DVCPRO HD 1080i59.94", 1280, 1080, 30000, 1001, "upper", 1.5, "dv100", 100, "4:2:2"},
            {"DVCPRO HD 1080i50", 1440, 1080, 25, 1, "upper", 1.3333, "dv100", 100, "4:2:2"},
            {"DVCPRO HD 720p59.94", 960, 720, 60000, 1001, "none", 1.3333, "dv100", 100, "4:2:2"},
            {"DVCPRO HD 720p50", 960, 720, 50, 1, "none", 1.3333, "dv100", 100, "4:2:2"}};
        for (const Dv& r : dv) {
            Tmpl t = make(r.name, r.w, r.h, r.num, r.den, r.mbps * 1000, r.mbps * 1000,
                          QString("Audio: 48,000 Hz, 16 Bit, Stereo, PCM. Video: %1x%2, %3 fps, %4, %5 Mbps %6.").arg(r.w).arg(r.h)
                              .arg((double)r.num / r.den, 0, 'f', 3).arg(r.sizeText).arg(r.mbps).arg(QString(r.name).startsWith("DVCPRO HD") ? "DVCPRO HD" : "DV"));
            t.rc = "cbr";
            t.field = r.field;
            t.par = r.par;
            t.profile = r.profile;
            t.picture = "dv";
            f.templates.push_back(t);
        }
        struct Ai { int cls; int w, h; const char* name; int num, den; const char* field; };
        // class 100: 1920x1080 / 1280x720, 4:2:2; class 50: 1440x1080 / 960x720 anamorphic, 4:2:0; both 10-bit, intra only
        for (int cls : {100, 50}) {
            const bool full = cls == 100;
            const Ai rows[] = {{cls, full ? 1920 : 1440, 1080, "1080i59.94", 30000, 1001, "upper"}, {cls, full ? 1920 : 1440, 1080, "1080i50", 25, 1, "upper"},
                               {cls, full ? 1920 : 1440, 1080, "1080p29.97", 30000, 1001, "none"}, {cls, full ? 1920 : 1440, 1080, "1080p25", 25, 1, "none"},
                               {cls, full ? 1920 : 1440, 1080, "1080p23.976", 24000, 1001, "none"}, {cls, full ? 1280 : 960, 720, "720p59.94", 60000, 1001, "none"},
                               {cls, full ? 1280 : 960, 720, "720p50", 50, 1, "none"}};
            for (const Ai& r : rows) {
                Tmpl t = make(QString("AVC-Intra %1 %2").arg(cls).arg(r.name), r.w, r.h, r.num, r.den, cls * 1000, cls * 1000,
                              QString("Audio: 48,000 Hz, 24 Bit, Stereo, PCM. Video: %1x%2, %3 fps, AVC-Intra class %4 (%5 10-bit intra frames, about %4 Mbps).")
                                  .arg(r.w).arg(r.h).arg((double)r.num / r.den, 0, 'f', 3).arg(cls).arg(full ? "4:2:2" : "4:2:0"));
                t.rc = "cbr";
                t.field = r.field;
                t.par = r.w == 1440 || r.w == 960 ? 1.3333 : 1.0;
                t.profile = full ? "high422" : "high10";
                t.picture = "h264";
                t.bits = 24;
                t.keySeconds = 0;
                t.needsOption = "avcintra-class";
                t.ffOptions = QString("x264-params=avcintra-class=%1%2").arg(cls).arg(QString(r.field) == "upper" ? ":tff=1" : "");
                f.templates.push_back(t);
            }
        }
        all.push_back(f);
    }
    {
        // Voukoder is a third-party VEGAS encoder plug-in that exposes FFmpeg's encoders and options. This is the
        // same idea:
        // encoders by name, each in a suitable container, with options in Customize Template > Video > Encoder options.
        Format f;
        f.key = "ffmpeg";
        f.name = "FFmpeg (Voukoder-style)";
        f.ext = "mp4";
        f.blurb = "Not a VEGAS format: FFmpeg's encoders by name, each in a suitable container, with their encoder options open to edit "
                  "(what the Voukoder plug-in offers VEGAS).";
        f.videoFamily = "h264";
        f.audioFamily = "aac";
        f.alphaPossible = true;
        f.profiles = QStringList() << "baseline|Baseline" << "main|Main" << "high|High" << "main10|Main 10 (HEVC)" << "high10|High 10 (H.264)"
                                   << "hq|ProRes 422 HQ" << "4444|ProRes 4444" << "dnxhr_hq|DNxHR HQ";
        struct Row { const char* name; const char* picture; const char* ext; const char* profile; const char* sound; int quality; const char* options; bool alpha; const char* desc; };
        const Row rows[] = {
            {"H.264 (x264) quality 18, MP4", "h264", "mp4", "high", "aac", 18, "preset=slow", false, "x264 at constant quality 18 (near transparent), slow preset, AAC sound."},
            {"H.264 (x264) quality 23, MP4", "h264", "mp4", "high", "aac", 23, "preset=medium", false, "x264 at constant quality 23 (x264's default), medium preset, AAC sound."},
            {"H.264 10-bit (x264) quality 18, MKV", "h264", "mkv", "high10", "aac", 18, "preset=slow", false, "x264 in 10-bit at constant quality 18, AAC sound."},
            {"H.265 (x265) quality 22, MP4", "hevc", "mp4", "main", "aac", 22, "preset=medium", false, "x265 at constant quality 22, medium preset, AAC sound."},
            {"H.265 10-bit (x265) quality 20, MKV", "hevc", "mkv", "main10", "aac", 20, "preset=slow", false, "x265 in 10-bit at constant quality 20, slow preset, AAC sound."},
            {"VP9, good quality, WebM", "vp9", "webm", "", "opus", 22, "", false, "VP9 at constant quality, Opus sound."},
            {"AV1, good quality, MKV", "av1", "mkv", "", "opus", 24, "", false, "AV1 at constant quality, Opus sound."},
            {"ProRes 422 HQ, MOV", "prores", "mov", "hq", "pcm", 0, "", false, "Apple ProRes 422 HQ pictures and 24-bit PCM sound, for editing elsewhere."},
            {"ProRes 4444 with alpha, MOV", "prores", "mov", "4444", "pcm", 0, "", true, "Apple ProRes 4444 pictures that keep transparency, and 24-bit PCM sound."},
            {"DNxHR HQ, MOV", "dnxhd", "mov", "dnxhr_hq", "pcm", 0, "", false, "Avid DNxHR HQ pictures (any size) and 24-bit PCM sound, for editing elsewhere."},
            {"FFV1 lossless, MKV", "ffv1", "mkv", "", "flac", 0, "", false, "Lossless FFV1 pictures and FLAC sound, for keeping."},
            {"UT Video lossless, AVI", "utvideo", "avi", "", "pcm", 0, "", false, "Lossless UT Video pictures and 24-bit PCM sound."},
            {"H.264 (x264), your options, MP4", "h264", "mp4", "high", "aac", 20, "preset=slow tune=film", false,
             "x264 with the encoder options of your choice: Customize Template > Video > Encoder options (FFmpeg's key=value words)."}};
        for (const Row& r : rows) {
            Tmpl t = make(r.name, 0, 0, 0, 1, 16000, 24000, r.desc);
            t.picture = r.picture;
            t.ext = r.ext;
            t.profile = r.profile;
            t.sound = QString(r.sound) == "aac" ? QString() : QString(r.sound);
            t.audioKbps = 256;
            t.sampleRate = 0;
            t.par = 0;
            t.field = "";
            t.alpha = r.alpha;
            t.ffOptions = r.options;
            if (r.quality > 0) {
                t.rc = "quality";
                t.quality = r.quality;
            }
            if (QString(r.sound) == "pcm") t.bits = 24;
            const QString pic = r.picture;
            if (pic == "prores" || pic == "dnxhd" || pic == "ffv1" || pic == "utvideo") t.kbps = t.maxKbps = 0;
            f.templates.push_back(t);
        }
        all.push_back(f);
    }
    {
        // Sony Perfect Clarity Audio (.pca): Sony's lossless audio codec (SoundForge, VEGAS). The coding is unpublished and
        // FFmpeg has no encoder, so Reno lists it greyed and says what to use instead.
        Format f;
        f.key = "pca";
        f.name = "Sony Perfect Clarity Audio";
        f.ext = "pca";
        f.hasVideo = false;
        f.blurb = "Sony's lossless audio format (.pca).";
        f.unavailable = "Sony Perfect Clarity Audio (.pca) is Sony's own lossless audio codec. Its coding is not published and FFmpeg has no encoder for it, "
                        "so Reno cannot write it. FLAC Audio and Apple Lossless (ALAC) are lossless too: render to one of those.";
        all.push_back(f);
    }
    for (Format& f : all)
        for (Tmpl& t : f.templates)
            if (t.width == 0 && t.height == 0) {
                t.par = 0;
                t.field = "";
            }
    std::vector<Format> have;
    for (Format& f : all) {
        std::vector<Tmpl> ok;
        for (const Tmpl& t : f.templates)
            if (templateAvailable(f, t)) ok.push_back(t);
        if (ok.empty() && f.unavailable.isEmpty()) continue;
        f.templates = ok;
        have.push_back(f);
    }
    return have;
}

std::string videoFamilyOf(const Format& f, const Tmpl& t)
{
    if (t.picture.isEmpty()) return f.videoFamily;
    return t.picture == "jpeg" ? std::string("mjpeg") : t.picture.toStdString();
}

std::string audioFamilyOf(const Format& f, const Tmpl& t)
{
    if (!t.sound.isEmpty()) return t.sound.toStdString();
    if (f.key == "mov" && t.picture == "h264") return "aac";
    return f.audioFamily;
}

bool templateAvailable(const Format& f, const Tmpl& t)
{
    static std::map<std::string, bool> known;
    auto ask = [](const std::string& key, const std::function<bool()>& how) {
        auto it = known.find(key);
        if (it != known.end()) return it->second;
        if (hardwareQueryHook()) hardwareQueryHook()(true);
        const bool ok = how();
        if (hardwareQueryHook()) hardwareQueryHook()(false);
        known[key] = ok;
        return ok;
    };
    const std::string ext = (f.sequence ? pictureExt(t) : fileExt(f, &t)).toStdString();
    if (!ask("ext:" + ext, [&] { return containerAvailable(ext); })) return false;
    if (f.hasVideo && t.video) {
        const std::string fam = videoFamilyOf(f, t);
        if (!fam.empty() && !ask("video:" + fam, [&] { return videoFamilyAvailable(fam); })) return false;
        if (!t.needsOption.isEmpty() && !ask("option:" + fam + ":" + t.needsOption.toStdString(), [&] { return videoEncoderHasOption(fam, t.needsOption.toStdString()); })) return false;
    }
    if (f.hasAudio && t.audio) {
        const std::string fam = audioFamilyOf(f, t);
        if (!fam.empty() && !ask("audio:" + fam, [&] { return audioFamilyAvailable(fam); })) return false;
    }
    return true;
}

QString templatesFile()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.reno";
    QDir().mkpath(dir);
    return QDir(dir).filePath("render_templates.json");
}

std::vector<Format> loadFormats()
{
    std::vector<Format> all = builtInFormats();
    QFile f(templatesFile());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        for (Format& fmt : all) {
            const QJsonArray list = root.value(fmt.key).toArray();
            Tmpl base = fmt.templates.empty() ? Tmpl() : fmt.templates.front();
            base.description.clear();
            std::vector<Tmpl> mine;
            for (const QJsonValue& v : list) {
                if (!v.isObject()) continue;
                Tmpl t = Tmpl::fromJson(v.toObject(), base);
                if (t.name.trimmed().isEmpty()) continue;
                mine.push_back(t);
            }
            fmt.templates.insert(fmt.templates.begin(), mine.begin(), mine.end());
        }
    }
    return all;
}

void saveCustomTemplates(const std::vector<Format>& all)
{
    QJsonObject root;
    {
        QFile old(templatesFile());
        if (old.open(QIODevice::ReadOnly)) {
            const QJsonObject before = QJsonDocument::fromJson(old.readAll()).object();
            for (auto it = before.begin(); it != before.end(); ++it) {
                bool loaded = false;
                for (const Format& fmt : all) loaded = loaded || fmt.key == it.key();
                if (!loaded) root[it.key()] = it.value();
            }
        }
    }
    for (const Format& fmt : all) {
        QJsonArray list;
        for (const Tmpl& t : fmt.templates)
            if (t.custom && t.name != "(Untitled)") list.append(t.toJson());
        if (!list.isEmpty()) root[fmt.key] = list;
    }
    QFile f(templatesFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(root).toJson());
}

QString fpsLabel(int num, int den)
{
    if (num <= 0) return "the project's rate";
    const double fps = (double)num / std::max(1, den);
    return QString::number(fps, 'f', 3);
}

static bool bitRatePicture(const Tmpl& t)
{
    return t.picture.isEmpty() || t.picture == "h264" || t.picture == "hevc" || t.picture == "vp8" || t.picture == "vp9" || t.picture == "av1" ||
           t.picture == "theora";
}

QString templateInfo(const Format& f, const Tmpl& t)
{
    QStringList lines;
    if (!t.description.isEmpty()) lines << QString("Description: %1").arg(t.description);
    if (f.hasAudio && t.audio) {
        QString a = t.sampleRate > 0 ? QString("%1 Hz").arg(thousands(t.sampleRate)) : QString("project sample rate");
        if (f.audioRate) a = QString("%1, %2").arg(rateText(t.audioKbps), a);
        else a += QString(", %1 Bit%2").arg(t.bits).arg(t.bits == 32 ? " (float)" : "");
        a += t.channels == 1 ? ", Mono" : (t.channels == 6 ? ", 5.1 Surround" : ", Stereo");
        const std::string af = audioFamilyOf(f, t);
        const QString codec = af == "aac" ? "AAC" : af == "mp3" ? "MP3" : af == "pcm" ? "PCM" : af == "flac" ? "FLAC" :
                              af == "vorbis" ? "Vorbis" : af == "opus" ? "Opus" : af == "wma" ? "WMA" :
                              af == "ac3" ? "Dolby Digital AC-3" : af == "alac" ? "ALAC" : af == "mp2" ? "MPEG-1 Layer II" : "";
        if (!codec.isEmpty()) a += ", " + codec;
        lines << "Audio: " + a;
    }
    if (f.hasVideo && t.video) {
        QString v = t.fpsNum > 0 ? QString("%1 fps").arg(fpsLabel(t.fpsNum, t.fpsDen)) : QString("project frame rate");
        v += ", " + (t.width > 0 ? QString("%1x%2").arg(t.width).arg(t.height) : QString("project frame size"));
        v += t.field == "upper" ? " Upper field first" : t.field == "lower" ? " Lower field first" : t.field.isEmpty() ? QString() : QString(" Progressive");
        if (t.picture == "qtrle") v += ", Animation (QTRLE)";
        else if (!t.picture.isEmpty() && t.picture != "h264") v += ", " + t.picture.toUpper();
        else if (f.videoFamily == "prores") v += ", " + (t.profile == "4444" ? QString("ProRes 4444") : QString("ProRes 422 %1").arg(t.profile.toUpper()));
        else if (!f.videoFamily.empty()) v += ", " + qs(f.videoFamily).toUpper();
        if (f.bitRate && bitRatePicture(t)) {
            if (t.rc == "quality") v += QString(", constant quality %1").arg(t.quality);
            else if (t.rc == "cbr") v += QString(", %1 CBR").arg(rateText(t.kbps));
            else v += QString(", %1%2").arg(rateText(t.kbps)).arg(t.maxKbps > t.kbps ? QString(" (%1 peak)").arg(rateText(t.maxKbps)) : QString());
            if (t.twoPass) v += ", two-pass";
            if (t.smartRender) v += ", smart render";
        }
        if (t.alpha) v += ", with alpha";
        if (t.mode != "auto" && !t.mode.isEmpty()) v += QString(", %1").arg(t.mode == "cpu" ? "CPU only" : modeLabel(t.mode));
        lines << "Video: " + v;
        if (t.par > 0) lines << QString("Pixel Aspect Ratio: %1").arg(t.par, 0, 'f', 3);
    }
    return lines.join("\n");
}

qint64 estimateBytes(const Format& f, const Tmpl& t, const ProjectSettings& ps, double seconds)
{
    if (seconds <= 0) return 0;
    double bits = 0;
    const int w = t.width > 0 ? t.width : ps.width, h = t.height > 0 ? t.height : ps.height;
    const double fps = t.fpsNum > 0 ? (double)t.fpsNum / t.fpsDen : ps.fps();
    if (f.hasVideo && t.video) {
        if (f.bitRate && bitRatePicture(t)) {
            if (t.rc == "quality") return -1;
            bits += t.kbps * 1000.0 * seconds;
        } else if (t.picture == "dv") {
            bits += 25e6 * seconds;
        } else if (t.picture.isEmpty() && f.videoFamily == "rawvideo") {
            bits += (double)w * h * (t.alpha ? 32 : 24) * fps * seconds;
        } else {
            return -1;
        }
    }
    if (f.hasAudio && t.audio) {
        const int rate = t.sampleRate > 0 ? t.sampleRate : ps.sampleRate;
        if (f.audioRate) bits += t.audioKbps * 1000.0 * seconds;
        else if (f.audioFamily == "flac") bits += rate * 2.0 * t.channels * t.bits * 0.6 * seconds;
        else bits += (double)rate * t.channels * t.bits * seconds;
    }
    return (qint64)(bits / 8.0);
}

QString pictureExt(const Tmpl& t)
{
    if (t.picture == "jpeg") return "jpg";
    if (t.picture == "tiff") return "tif";
    if (t.picture == "bmp") return "bmp";
    if (t.picture == "exr") return "exr";
    if (t.picture == "tga") return "tga";
    if (t.picture == "webp") return "webp";
    return "png";
}

bool makeRenderSpec(const Format& f, const Tmpl& t, const RenderChoices& c, const ProjectSettings& ps, RenderSpec* out, QString* error)
{
    RenderSpec spec;
    EncodeSettings& es = spec.enc;
    es.path = c.writerPath;
    es.video = f.hasVideo && t.video;
    es.audio = f.hasAudio && t.audio;
    if (!es.video && !es.audio) {
        if (error) *error = "This template has neither video nor audio. Customize it to include one.";
        return false;
    }
    es.videoFamily = videoFamilyOf(f, t);
    es.audioFamily = audioFamilyOf(f, t);
    es.encodeMode = t.mode.toStdString();
    es.videoKbps = t.kbps;
    es.maxKbps = t.maxKbps;
    es.rateControl = f.bitRate ? t.rc.toStdString() : "vbr";
    es.quality = t.quality;
    es.profile = t.profile.toStdString();
    es.level = t.level.toStdString();
    es.encoderOptions = t.ffOptions.toStdString();
    es.keyframeSeconds = t.keySeconds;
    es.audioKbps = t.audioKbps;
    es.audioBits = t.bits;
    es.audioChannels = t.channels;
    es.alpha = t.alpha;
    es.fieldOrder = t.field.isEmpty() ? ps.fieldOrder : t.field.toStdString();
    es.pixelAspect = t.par > 0 ? t.par : ps.pixelAspect;
    es.stretch = c.stretch;
    spec.width = t.width;
    spec.height = t.height;
    spec.fpsNum = t.fpsNum;
    spec.fpsDen = t.fpsDen;
    spec.sampleRate = t.sampleRate;
    spec.quality = t.renderQuality.toStdString();
    spec.motionBlur = t.motionBlur.toStdString();
    spec.useRotation = c.useRotation;
    spec.markers = c.markers;
    spec.twoPass = t.twoPass && f.bitRate && t.rc == "vbr";
    spec.smartRender = t.smartRender && f.bitRate;
    spec.transparent = t.alpha;
    spec.loudnessLog = c.loudnessLog;
    spec.loudnessAtsc = c.loudnessAtsc;
    spec.colorSpace = t.colorSpace.toStdString();
    spec.multipleMono = t.multipleMono && !es.video;
    if (takesChannelMapping(f) && !t.multipleMono) spec.channelMap = c.channelMap;
    spec.spherical = ps.output360 && es.video;
    if (c.summary) {
        if (!ps.title.empty()) es.metadata.push_back({"title", ps.title});
        if (!ps.artist.empty()) es.metadata.push_back({"artist", ps.artist});
        if (!ps.copyright.empty()) es.metadata.push_back({"copyright", ps.copyright});
        if (!ps.engineer.empty()) es.metadata.push_back({"encoded_by", ps.engineer});
    }
    std::string comment = c.summary ? ps.comments : std::string();
    if (!c.projectPath.empty()) comment += (comment.empty() ? "" : "\n") + std::string("Project: ") + c.projectPath;
    if (!comment.empty()) es.metadata.push_back({"comment", comment});
    if (out) *out = spec;
    return true;
}

bool takesChannelMapping(const Format& f)
{
    return f.audioFamily == "pcm" && (f.key == "wav" || f.key == "w64" || f.key == "avi" || f.key.startsWith("mxf"));
}

bool findTemplate(const std::vector<Format>& all, const QString& name, int* formatIndex, int* templateIndex, QString* error)
{
    auto same = [](const QString& a, const QString& b) { return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0; };
    for (int cut = name.indexOf('/'); cut > 0; cut = name.indexOf('/', cut + 1)) {
        const QString fn = name.left(cut), tn = name.mid(cut + 1);
        for (size_t fi = 0; fi < all.size(); ++fi) {
            if (!same(all[fi].name, fn) && !same(all[fi].key, fn)) continue;
            for (size_t ti = 0; ti < all[fi].templates.size(); ++ti)
                if (same(all[fi].templates[ti].name, tn)) {
                    if (formatIndex) *formatIndex = (int)fi;
                    if (templateIndex) *templateIndex = (int)ti;
                    return true;
                }
        }
    }
    std::vector<std::pair<int, int>> found;
    for (size_t fi = 0; fi < all.size(); ++fi)
        for (size_t ti = 0; ti < all[fi].templates.size(); ++ti)
            if (same(all[fi].templates[ti].name, name)) {
                found.push_back({(int)fi, (int)ti});
                break;
            }
    if (found.size() == 1) {
        if (formatIndex) *formatIndex = found[0].first;
        if (templateIndex) *templateIndex = found[0].second;
        return true;
    }
    if (error) {
        if (found.empty()) {
            *error = QString("There is no render template called \"%1\".").arg(name);
        } else {
            QStringList names;
            for (const auto& f : found) names << all[(size_t)f.first].name + "/" + name;
            *error = QString("Several formats have a template called \"%1\"; name the format too: %2").arg(name, names.join(", "));
        }
    }
    return false;
}

}  // namespace rt
}  // namespace app
}  // namespace reno
