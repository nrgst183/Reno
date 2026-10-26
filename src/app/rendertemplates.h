// File > Render As's formats and templates (VEGAS Pro 18's), the saved custom templates, and how a template plus
// Render As's options become a RenderSpec for renderProjectAs (engine/renderas.h). Shared by the Render As dialog and
// reno_cli. Qt Core only.
#pragma once
#include "../engine/renderas.h"
#include "../media/media.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace reno {
namespace app {
namespace rt {

// One Render As template: everything Customize Template sets. Sizes and rates of 0 mean "the project's".
struct Tmpl {
    QString name, description;
    bool custom = false;
    bool video = true;
    int width = 0, height = 0;
    int fpsNum = 0, fpsDen = 1;
    QString field = "none";             // none, upper, lower ("" = the project's)
    double par = 1.0;                   // 0 = the project's
    QString profile, level;
    QString rc = "vbr";                 // vbr, cbr, quality
    int kbps = 16000, maxKbps = 24000;
    int quality = 23;
    bool twoPass = false;
    QString mode = "auto";              // auto, cpu, nvenc, qsv, amf, mf
    double keySeconds = 2.0;
    bool alpha = false;
    // image sequences: png, jpeg, tiff, bmp, exr, tga, webp; otherwise the video compression where it isn't the
    // format's own (h264, vp8, av1, qtrle...)
    QString picture;
    bool audio = true;
    int sampleRate = 48000;             // 0 = the project's
    int audioKbps = 192;
    int channels = 2;
    int bits = 16;
    QString sound;  // the audio compression where it isn't the format's own (vorbis, opus...)
    QString renderQuality;              // "" = the project's; draft, preview, good, best
    QString motionBlur;                 // "" = the project's
    QString colorSpace;  // a linear-light project's color space for the file ("" = Default: the View transform; colormgmt.h)
    bool multipleMono = false;          // VEGAS's multiple mono: each of the master's channels in a mono file of its own
    // the file's extension where it isn't the format's (FFmpeg templates: mp4, mov, mkv, webm...); encoder options in
    // FFmpeg's words ("preset=slow tune=film x265-params=aq-mode=3") added to the video encoder's; and an encoder
    // option the template needs this FFmpeg's encoder to have
    QString ext, ffOptions, needsOption;

    QJsonObject toJson() const
    {
        QJsonObject o;
        o["name"] = name;
        o["description"] = description;
        o["video"] = video;
        o["width"] = width;
        o["height"] = height;
        o["fpsNum"] = fpsNum;
        o["fpsDen"] = fpsDen;
        o["field"] = field;
        o["par"] = par;
        o["profile"] = profile;
        o["level"] = level;
        o["rc"] = rc;
        o["kbps"] = kbps;
        o["maxKbps"] = maxKbps;
        o["quality"] = quality;
        o["twoPass"] = twoPass;
        o["mode"] = mode;
        o["keySeconds"] = keySeconds;
        o["alpha"] = alpha;
        o["picture"] = picture;
        o["audio"] = audio;
        o["sampleRate"] = sampleRate;
        o["audioKbps"] = audioKbps;
        o["channels"] = channels;
        o["bits"] = bits;
        o["sound"] = sound;
        o["renderQuality"] = renderQuality;
        o["motionBlur"] = motionBlur;
        if (!colorSpace.isEmpty()) o["colorSpace"] = colorSpace;
        if (multipleMono) o["multipleMono"] = true;
        if (!ext.isEmpty()) o["ext"] = ext;
        if (!ffOptions.isEmpty()) o["ffOptions"] = ffOptions;
        if (!needsOption.isEmpty()) o["needsOption"] = needsOption;
        return o;
    }
    static Tmpl fromJson(const QJsonObject& o, const Tmpl& base)
    {
        Tmpl t = base;
        t.name = o.value("name").toString(base.name);
        t.description = o.value("description").toString(base.description);
        t.video = o.value("video").toBool(base.video);
        t.width = std::max(0, std::min(16384, o.value("width").toInt(base.width)));
        t.height = std::max(0, std::min(16384, o.value("height").toInt(base.height)));
        t.fpsNum = std::max(0, o.value("fpsNum").toInt(base.fpsNum));
        t.fpsDen = std::max(1, o.value("fpsDen").toInt(base.fpsDen));
        t.field = o.value("field").toString(base.field);
        t.par = std::max(0.0, std::min(10.0, o.value("par").toDouble(base.par)));
        t.profile = o.value("profile").toString(base.profile);
        t.level = o.value("level").toString(base.level);
        t.rc = o.value("rc").toString(base.rc);
        t.kbps = std::max(32, std::min(2000000, o.value("kbps").toInt(base.kbps)));
        t.maxKbps = std::max(0, std::min(2000000, o.value("maxKbps").toInt(base.maxKbps)));
        t.quality = std::max(0, std::min(51, o.value("quality").toInt(base.quality)));
        t.twoPass = o.value("twoPass").toBool(base.twoPass);
        t.mode = o.value("mode").toString(base.mode);
        t.keySeconds = std::max(0.0, std::min(600.0, o.value("keySeconds").toDouble(base.keySeconds)));
        t.alpha = o.value("alpha").toBool(base.alpha);
        t.picture = o.value("picture").toString(base.picture);
        t.audio = o.value("audio").toBool(base.audio);
        t.sampleRate = std::max(0, std::min(384000, o.value("sampleRate").toInt(base.sampleRate)));
        t.audioKbps = std::max(8, std::min(1536, o.value("audioKbps").toInt(base.audioKbps)));
        {
            const int ch = o.value("channels").toInt(base.channels);
            t.channels = ch == 1 ? 1 : (ch == 6 ? 6 : 2);
        }
        t.bits = o.value("bits").toInt(base.bits);
        if (t.bits != 16 && t.bits != 24 && t.bits != 32) t.bits = 16;
        t.sound = o.value("sound").toString(base.sound);
        t.renderQuality = o.value("renderQuality").toString(base.renderQuality);
        t.motionBlur = o.value("motionBlur").toString(base.motionBlur);
        t.colorSpace = o.value("colorSpace").toString(base.colorSpace);
        t.multipleMono = o.value("multipleMono").toBool(base.multipleMono);
        t.ext = o.value("ext").toString(base.ext);
        t.ffOptions = o.value("ffOptions").toString(base.ffOptions);
        t.needsOption = o.value("needsOption").toString(base.needsOption);
        t.custom = true;
        return t;
    }
};

struct Format {
    QString key;
    QString name;
    QString ext;
    QString blurb;
    std::string videoFamily, audioFamily;
    bool hasVideo = true, hasAudio = true;
    bool bitRate = true;
    bool audioRate = true;
    bool alphaPossible = false;
    bool sequence = false;
    QStringList profiles;               // the Profile list (value|label)
    std::vector<Tmpl> templates;
    // Not empty: a format VEGAS lists that Reno can't make; listed greyed with this as the reason (no templates)
    QString unavailable;
};

inline QString fileExt(const Format& f, const Tmpl* t) { return t && !t->ext.isEmpty() ? t->ext : f.ext; }

struct HardwareInfo {
    std::map<std::string, std::vector<EncoderMode>> modes;
    bool works(const std::string& family, const std::string& mode) const
    {
        if (mode == "qsv" && !qsvEncodingAllowed()) return false;  // Preferences > General: Enable QSV Encoding and Decoding
        auto it = modes.find(family);
        if (it == modes.end()) return false;
        for (const EncoderMode& m : it->second)
            if (m.mode == mode) return m.works;
        return false;
    }
    bool present(const std::string& family, const std::string& mode) const
    {
        auto it = modes.find(family);
        if (it == modes.end()) return false;
        for (const EncoderMode& m : it->second)
            if (m.mode == mode) return m.present;
        return false;
    }
};

// The hardware encoders this FFmpeg has that also start on this computer (asked once: each is tried on a small image).
// hardwareQueryHook(), if set, is called with true before asking and false after (the dialog shows the wait cursor).
const HardwareInfo& hardware();
std::function<void(bool busy)>& hardwareQueryHook();
const char* modeLabel(const QString& mode);                 // "NVENC", "Intel QSV", "AMD VCE", "Media Foundation" ("" others)

std::vector<Format> builtInFormats();
// Whether this FFmpeg can make files with 'f' / 't': it has a writer for the extension and encoders for the video and
// audio (a graphics card's encoder only if it starts here).
bool templateAvailable(const Format& f, const Tmpl& t);
std::string videoFamilyOf(const Format& f, const Tmpl& t);
std::string audioFamilyOf(const Format& f, const Tmpl& t);
QString templatesFile();
std::vector<Format> loadFormats();
void saveCustomTemplates(const std::vector<Format>& all);

QString thousands(int v);
QString rateText(int kbps);
QString fpsLabel(int num, int den);
QString templateInfo(const Format& f, const Tmpl& t);
qint64 estimateBytes(const Format& f, const Tmpl& t, const ProjectSettings& ps, double seconds);

QString pictureExt(const Tmpl& t);

struct RenderChoices {
    std::string writerPath;
    bool stretch = false;
    bool useRotation = false;
    bool markers = false;
    std::string loudnessLog;        // "Generate loudness log": the log's path (empty: none)
    bool summary = false;
    std::string projectPath;
    bool loudnessAtsc = false;
    std::vector<RenderSpec::ChannelSource> channelMap;     // "Enable multichannel mapping": the file's channels (empty: off)
};
// Whether a format can take Render As's multichannel mapping (PCM in WAV, Wave64, AVI, MXF).
bool takesChannelMapping(const Format& f);

// The RenderSpec File > Render As renders 'f' / 't' with. False with *error if there is nothing to render (a template
// with neither video nor audio).
bool makeRenderSpec(const Format& f, const Tmpl& t, const RenderChoices& c, const ProjectSettings& ps, RenderSpec* spec, QString* error);

// Finds a template by name: "Format/Template" (VEGAS's format name or Reno's key for it, as in render_templates.json:
// avc, hevc, wmv, avi, mov, mxf, xavcs, wav, w64, mp3, flac, ogg, m4a, ac3, wma, images, mkv, webm, ogv, opus, gif,
// apng, aiff, alac), or the template's name alone when only one format has it. Case-insensitive. False with *error
// (saying why) otherwise.
bool findTemplate(const std::vector<Format>& all, const QString& name, int* formatIndex, int* templateIndex, QString* error);

}  // namespace rt
}  // namespace app
}  // namespace reno
