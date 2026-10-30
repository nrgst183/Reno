// Puts Reno's own preferences (vprefs.h, and the VEGAS ones that set internal behavior) into effect in the parts that can't
// read QSettings: media code, engine, plug-in host. AppState::applyPreferences calls apply() when preferences load or
// change; main() calls earlyEnvironment() before the application object exists. Header-only, like vprefs.h.
#pragma once
#include "vprefs.h"
#include "mediacache.h"
#include "autoproxy.h"
#include "../core/base.h"
#include "../engine/engine.h"
#include "../engine/catalog.h"
#include "../engine/project.h"
#include "../audiofx/audioplugin.h"
#include "../media/media.h"
#include "../media/vegasfiles.h"
#include "../ofx/ofxhost.h"
#include "../ofx/vkgpu.h"
#include "../ofx/gpupick.h"
#include <QByteArray>
#include <QDir>
#include <QGuiApplication>
#include <QSettings>
#include <algorithm>

namespace reno {
namespace app {
namespace prefsapply {

// A folder setting as the media code wants it (forward slashes, no trailing slash; empty stays empty).
inline std::string folderOf(const QString& key)
{
    QString d = QDir::fromNativeSeparators(vprefs::text(key).trimmed());
    while (d.size() > 1 && d.endsWith('/')) d.chop(1);
    return d.toStdString();
}

// How many processors helper threads, effects and video codecs may use: 0 is all.
inline int processorLimit()
{
    return vprefs::on("prefs/useAllCores") ? 0 : std::max(1, std::min(256, vprefs::number("prefs/maxThreads")));
}

inline size_t audioCacheBytes() { return (size_t)std::max(0, std::min(16384, vprefs::number("prefs/audioCacheMb"))) * 1024u * 1024u; }

// Whether a video entering the project gets a proxy at once (Preferences > Video: "Automatically create video proxies
// for greater than 4K resolution media"; Preferences > Reno > Files: the same for long-GOP media from a given width).
inline bool isLongGopCodec(const std::string& codec)
{
    for (const char* c : {"h264", "hevc", "h265", "mpeg2video", "mpeg2", "vp9", "vp8", "av1"})
        if (codec == c) return true;
    return false;
}
inline bool wantsAutoProxy(const MediaInfo& mi)
{
    if (!mi.hasVideo || mi.isStill || !(mi.duration > 0) || mi.width <= 0 || mi.height <= 0) return false;
    if (vprefs::on("prefs/autoProxy4k") && (mi.width > 3840 || mi.height > 2160)) return true;
    return vprefs::on("prefs/autoProxyLongGop") && mi.width >= std::max(16, vprefs::number("prefs/autoProxyMinWidth")) && isLongGopCodec(mi.videoCodec);
}

// Plug-ins a project's effects need that this Reno can't offer (Preferences > Reno > Plug-ins, "Say which plug-ins
// are missing when a project opens"): OpenFX effects whose identifier is not in the plug-in list (or failed the
// check), and audio plug-ins whose file is missing. Each name once.
inline std::vector<std::string> missingPlugins(const Project& p)
{
    std::vector<std::string> out;
    auto add = [&out](const std::string& n) {
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    };
    const std::vector<AudioFxEntry> audio = Catalog::get().audio();
    auto check = [&](const FxChain& chain) {
        for (const FxSlot& s : chain) {
            if (s.kind == "ofx") {
                VideoFxEntry e;
                if (!Catalog::get().findVideo(s.pluginId, &e) || !e.ok) add(s.name.empty() ? s.pluginId : s.name);
            } else if (s.kind == "audio") {
                bool found = s.audio.format == "VST2" && efxPreferBuiltin() && efxReproducedId(s.audio.uid);
                for (const AudioFxEntry& a : audio)
                    if (a.ok && (a.desc.format == "BUILTIN" ? (a.desc.name == s.audio.name) : (!s.audio.path.empty() && a.desc.path == s.audio.path))) found = true;
                if (!found) add(s.name.empty() ? (s.audio.name.empty() ? s.audio.path : s.audio.name) : s.name);
            }
        }
    };
    for (const Track& t : p.tracks) {
        check(t.fx);
        for (const Event& e : t.events) check(e.fx);
    }
    check(p.videoOut);
    check(p.audioOut);
    for (const Bus& b : p.buses) check(b.fx);
    return out;
}

// Where video proxies go (media.h): the folder from Preferences, else Reno's cache folder, or (when asked) empty:
// beside the media, as VEGAS does.
inline std::string proxyFolderSetting()
{
    const std::string chosen = folderOf("prefs/proxyFolder");
    if (!chosen.empty() || vprefs::on("prefs/proxyBeside")) return chosen;
    return QDir::fromNativeSeparators(mediacache::folder("proxies")).toStdString();
}

// "GPU API" (Preferences > Video, reno_cli --gpu-api): vulkan - built-in effects with GPU kernels use Vulkan (plug-ins
// that can use OpenGL still do, if "GPU acceleration" is on); opengl - OpenGL only for plug-ins; off - CPU for everything.
inline void applyGpuApi(const std::string& api, bool gpuAccel)
{
    ofx::HostConfig& hc = ofx::Host::get().config;
    const bool openGl = api != "off" && gpuAccel;
    if (hc.gpu != openGl) {
        hc.gpu = openGl;
        ofx::Host::get().applyConfig();
    }
    gpu::setEnabled(api == "vulkan");
}

// "GPU effects" (Preferences > Video): auto - each effect goes the way that was measured to be quicker on this card (ofx/gpupick.h); gpu - always the card.
inline void applyGpuPick(const QString& mode) { ofx::builtin::pick::setAuto(mode != "gpu"); }

inline void apply()
{
    reno::setProcessorLimit(processorLimit());
    reno::setVideoDecoderThreadLimit(std::max(0, vprefs::number("prefs/decoderThreads")));
    AudioEngine::setEventCacheLimit(audioCacheBytes());
    reno::setVideoProxyFolder(proxyFolderSetting());
    if (autoproxy::Worker::get().preferencesControl())
        autoproxy::Worker::get().setModes(vprefs::on("prefs/autoProxyPreview"), vprefs::on("prefs/autoProxy4k"), vprefs::on("prefs/autoProxyLongGop"));
    vegasfiles::setPeakFolder(folderOf("prefs/peakFolder"));
    reno::setQsvEncodingAllowed(vprefs::on("prefs/qsv"));
    reno::setHardwareDecoderLimit(std::max(1, vprefs::number("prefs/hwDecoders")));
    reno::setHardwareDecoding(vprefs::text("prefs/hwDecode") == "off" ? reno::HwDecodeMode::Off : reno::HwDecodeMode::Auto,
                              vprefs::text("prefs/hwDecodeApi").trimmed().toStdString());
    // GPU acceleration: effects that can render with OpenGL (the host says so when this is on) and built-in effects
    // with Vulkan kernels (GPU API)
    applyGpuApi(vprefs::text("prefs/gpuApi").toStdString(), vprefs::on("prefs/gpuAccel"));
    applyGpuPick(vprefs::text("prefs/gpuPick"));
    // VEGAS's effects: its own plug-ins when installed, or always Reno's built-in versions (FxRuntime recreates the
    // instances)
    ofx::Host::get().config.preferBuiltinVegasFx = vprefs::text("prefs/vegasFxSource") != "real";
    // VEGAS's eFX audio effects: Reno's own versions (default) or the installed MAGIX plug-ins (FxRuntime recreates the
    // instances)
    reno::setEfxPreferBuiltin(vprefs::text("prefs/vegasEfxSource") != "real");
    // VEGAS's DirectX audio effects (Reverb, EQ, Track FX...): Reno's versions (default) or the installed plug-ins
    reno::setDxPreferBuiltin(vprefs::text("prefs/vegasDxSource") != "real");
    ofx::Host::get().config.previewSizeForAll = vprefs::on("prefs/previewSizeAllFx");
    reno::setProbeExeOverride(QDir::fromNativeSeparators(vprefs::text("prefs/renoCliPath").trimmed()).toStdString());
    ofx::Param::newKeyCurve().store(vprefs::curveCode("prefs/curveVideoFx"));
    mediacache::trim("thumbs", (qint64)std::max(0, vprefs::number("prefs/thumbCacheMb")) * 1024 * 1024);
    mediacache::trim("peaks", (qint64)std::max(0, vprefs::number("prefs/peakCacheMb")) * 1024 * 1024);
    mediacache::trim("proxies", (qint64)std::max(256, vprefs::number("prefs/proxyCacheMb")) * 1024 * 1024);
}

// Applies the interface size and High DPI settings (Preferences > Display and Preferences > Reno > Interface).
inline QStringList earlyEnvironment()
{
    QStringList set;
    auto put = [&set](const char* name, const QByteArray& value) {
        if (qEnvironmentVariableIsSet(name)) return;
        qputenv(name, value);
        set << QString("%1=%2").arg(name, QString::fromLatin1(value));
    };
    if (!vprefs::on("prefs/highDpi")) put("QT_ENABLE_HIGHDPI_SCALING", "0");
    const int scale = vprefs::number("prefs/uiScale");
    if (scale >= 50 && scale <= 300 && scale != 100) put("QT_SCALE_FACTOR", QByteArray::number(scale / 100.0, 'f', 2));
    if (vprefs::on("prefs/altHighDpi")) {
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
        set << "RoundPreferFloor";
    }
    return set;
}

}  // namespace prefsapply
}  // namespace app
}  // namespace reno
