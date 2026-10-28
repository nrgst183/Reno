// VEGAS Pro 18's preferences that Reno keeps beside its own Preferences struct (appstate.h): the items of VEGAS's
// Options > Preferences tabs that the Preferences dialog (prefsdialog.cpp) shows and the rest of Reno reads.
// 
// Each is a QSettings value ("prefs/<key>", or the key of the setting Reno already had for the same thing) with
// VEGAS's default. Reads go through a small cache (the timeline asks while painting) that set() and reload() keep
// current; Options > Import Preferences calls reload(). Header-only.
#pragma once
#include <QColor>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <map>

namespace reno {
namespace app {
namespace vprefs {

inline const std::map<QString, QVariant>& defaults()
{
    static const std::map<QString, QVariant> d = {
        {"prefs/openLastProject", false},           // Automatically open last project on startup
        {"prefs/confirmMediaDelete", true},         // Confirm media deletion when still in use
        {"prefs/savePrerenders", true},             // Save active prerenders on project close
        {"prefs/closeMediaInactive", false},        // Close media files when not the active application
        {"prefs/closePortsInactive", false},        // Close audio and MIDI ports when not the active application
        {"prefs/promptMatchMedia", true},           // Prompt to adjust project settings to match first media added to timeline
        {"prefs/wave64", true},                     // Render large Wave files as Wave64
        {"prefs/wave4gb", false},                   // Allow Wave renders up to 4 GB
        {"prefs/mxfMultichannel", true},            // Import MXF as multichannel
        {"prefs/stereoDualMono", false},            // Import stereo as dual mono
        {"prefs/qsv", true},  // Enable QSV Encoding and Decoding (where available)
        {"prefs/promptKeepRecorded", true},         // Prompt to keep files after recording
        {"prefs/fxUndo", true},                     // Create undos for FX parameter changes
        {"prefs/autoNameMarkers", true},            // Automatically name regions and markers if not playing
        {"prefs/spacePlayPause", false},            // Make spacebar and F12 Play/Pause instead of Play/Stop
        {"prefs/alwaysMarkerLines", true},  // Always draw marker lines
        {"prefs/dblClickTrimmer", true},  // Double-click on media file loads into Trimmer instead of tracks
        {"prefs/trimmerHistoryFileFirst", true},    // Show Trimmer history with filename first, then folder
        // Automatically save Trimmer markers and regions with media file (the Trimmer's Shift+S)
        {"trimmer/autoSaveMarkers", false},
        {"prefs/warnDeleteTrack", true},            // Provide a warning if a track being deleted holds at least one event
        {"prefs/recentList", true},                 // Recently used project list: [x] and the count
        {"prefs/recentCount", 4},
        {"prefs/tempFolder", QString()},            // Temporary files folder (empty: the system's)
        // ---- Video (beside Preferences' own: RAM preview, threads, thumbnails, safe areas, grid)
        // GPU acceleration of video processing: OpenFX effects that can render with OpenGL do (as VEGAS)
        {"prefs/gpuAccel", true},
        // GPU API for video processing: "vulkan" (built-in effects run on the card, plug-ins may use OpenGL),
        // "opengl" (plug-ins only) or "off"
        {"prefs/gpuApi", QString("vulkan")},
        // VEGAS's own effects: "real" (VEGAS's plug-ins when installed, else Reno's built-in versions) or "builtin"
        // (always Reno's)
        {"prefs/vegasFxSource", QString("builtin")},
        // VEGAS's eFX audio effects (Limiter, Compressor, Reverb, Tube Stage, Phaser, Chorus/Flanger): "builtin"
        // (Reno's own versions) or "real" (the installed MAGIX plug-ins: they need VEGAS's licence and run restricted
        // outside VEGAS)
        {"prefs/vegasEfxSource", QString("builtin")},
        {"prefs/vegasDxSource", QString("builtin")},
        // Smaller previews: every effect at the preview's size (as VEGAS), also those without multi-resolution
        {"prefs/previewSizeAllFx", true},
        {"prefs/autoProxy4k", true},                // Automatically create video proxies for greater than 4K resolution media
        {"prefs/previewBackground", QString("default")},    // Background color: default, black, white, checkerboard, or #rrggbb
        {"prefs/fadeTop", QString("#ffffff")},      // Default track fade colors: Track fade top
        {"prefs/fadeBottom", QString("#000000")},   // Track fade bottom
        {"prefs/pdDevice", QString("graphics")},    // graphics (Windows Graphics Card), or none
        {"prefs/pdAdapter", -1},                    // the screen (-1: one other than Reno's)
        {"prefs/pdScale", true},                    // Scale output to fit display
        {"prefs/pdDeinterlace", false},             // Apply deinterlace filter
        {"prefs/pdStudioRgb", false},               // Adjust levels from studio RGB to computer RGB
        {"prefs/pdShowInPreview", true},            // Display frames in video preview window during playback
        {"prefs/recordBwf", true},                  // Record to Broadcast Wave Format
        {"prefs/promptNoArmed", false},             // Prompt user when no track has been armed for record
        {"prefs/metronomeCustom", false},           // Metronome: Use custom metronome sounds
        {"prefs/metronomeNormal", QString()},
        {"prefs/metronomeAccent", QString()},
        {"prefs/loudnessOpenEditor", true},         // Open editor when new loudness log is generated
        {"prefs/playbackBuffer", 0.05},             // Playback buffering (seconds)
        {"prefs/autoRecordLatency", true},          // Automatically detect and offset for hardware recording latency
        {"prefs/recordOffsetMs", 0.0},              // User offset
        {"prefs/midiOutputs", QStringList()},       // the MIDI outputs / inputs made available (names); empty: all of them
        {"prefs/midiInputs", QStringList()},
        {"prefs/vstDisabled", QStringList()},       // the VST plug-ins (their files) not to be made available
        {"prefs/groupedCutCopy", false},            // Cut, copy, and delete grouped events
        {"prefs/collapseLoop", false},              // Collapse loop region when no time selection is present
        {"prefs/jklSpeed", QString("medium")},      // JKL / shuttle speed: slow, medium, fast
        {"prefs/overlapAlign", QString("center")},  // Cut-to-overlap conversion: Alignment: before, center, after
        {"prefs/autoCropStills", false},            // Automatically crop still images added to timeline
        {"prefs/smartSplitFrames", 5},              // Smart split default length (frames)
        {"prefs/trackColors", QStringList()},       // Track colors: the sixteen (empty: Reno's own)
        {"prefs/envelopeColors", QStringList()},    // Envelope colors, by kind ("kind=#rrggbb")
        {"prefs/snapColors", QStringList()},        // Snap colors, by kind
        {"prefs/timelineAtBottom", true},           // Display timeline at bottom of main window
        {"prefs/tabsAtTop", false},                 // Position tabs at top of docked windows
        {"prefs/floatingDock", true},               // Allow floating windows to dock (hold Ctrl to override)
        {"prefs/reverseEventBg", false},            // Reverse Event Background Color
        {"prefs/highDpi", true},                    // Use High DPI scaling (where available)
        {"prefs/altHighDpi", false},                // Use Alternate HighDPI Settings
        // ---- Sync (the devices are midisync's "midi/output" / "midi/input")
        // Generate MIDI Timecode: Frame rate (-1: the project's; else midisync's codes 0..3, 4 29.97 non-drop)
        {"midi/mtcRate", -1},
        {"midi/chaseRate", -1},                     // Trigger from MIDI Timecode: Frame rate
        // MTC Input: Free-wheel for timecode loss (VEGAS: on; off here, since Reno always stopped as soon as the
        // timecode did)
        {"midi/freewheel", false},
        {"midi/freewheelSlack", 0.5},
        {"midi/freewheelPlayback", 2.0},
        {"midi/syncDelay", 0.0},                    // (VEGAS: 1 s; 0 is how Reno always followed)
        {"midi/offsetQuarterFrames", 0},
        {"midi/triggerOnly", false},
        {"midi/fullFrame", QString("startStop")},   // MTC Output: never, startStop, all
        {"midi/clockStart", false},                 // MIDI Clock Output: Send Start instead of Continue
        {"midi/songPosition", QString("startStop")},    // never, startStop, all
        {"prefs/curveAudioEnv", QString("smooth")}, // New audio envelopes (fade type)
        {"prefs/curveAudioGain", QString("fast")},  // New audio event gain (the fade-in; the fade-out is its mirror)
        {"prefs/curveAudioFx", QString("linear")},  // New audio FX automation envelopes
        {"prefs/curveVideoEnv", QString("smooth")}, // New video envelopes
        {"prefs/curveVideoOpacity", QString("smooth")}, // New video event opacity
        {"prefs/curveVideoFx", QString("linear")},  // New video FX keyframes
        {"prefs/curveVideoMotion", QString("smooth")},  // New video motion keyframes
        {"prefs/motionSmoothness", 0.0},            // Track Motion default smoothness
        {"prefs/panCropSmoothness", 0.0},           // Pan/Crop default smoothness
        {"prefs/depXdcamExplorer", false},
        {"prefs/depDvdCamcorder", false},
        {"prefs/logPlugins", QString("off")},       // Log Plugin Loading / Reads / Renders: off, basic, detailed
        {"prefs/logReads", QString("off")},
        {"prefs/logRenders", QString("off")},
        // hardware decoding of video on the graphics card (reno::setHardwareDecoding): auto or off, which API (empty:
        // the usual order for the platform), and how many files may decode on the card at once. As in VEGAS, on by
        // default on Windows.
#ifdef _WIN32
        {"prefs/hwDecode", QString("auto")},
#else
        {"prefs/hwDecode", QString("off")},
#endif
        {"prefs/hwDecodeApi", QString()},
        {"prefs/hwDecoders", 8},
        {"prefs/useAllCores", true},  // helper threads, OpenFX effects and the video codecs may use every processor
        {"prefs/maxThreads", 4},  // ... or no more than this many when "use all" is cleared
        {"prefs/decoderThreads", 0},                // threads per video decoder (0: automatic)
        {"prefs/audioCacheMb", 256},  // time-stretched / pitch-shifted sound kept for playing (AudioEngine::setEventCacheLimit)
        {"prefs/thumbCacheMb", 512},  // thumbnails kept on disk between sessions (mediacache.h)
        {"prefs/peakCacheMb", 512},  // and the waveforms Reno keeps itself
        {"prefs/peakFolder", QString()},            // where VEGAS peak files (.sfk) are kept (empty: beside the media)
        {"prefs/thumbQuality", QString("normal")},  // video thumbnails: draft (half size), normal, high (twice)
        // preview proxies in the background (autoproxy.h) for the long-GOP video (H.264, HEVC, MPEG-2, VP9, AV1...)
        // the project uses
        {"prefs/autoProxyPreview", true},
        {"prefs/autoProxyLongGop", false},  // also: a video proxy of long-GOP media as it comes in (at once, used or not)...
        {"prefs/autoProxyMinWidth", 1920},          // ... when it is at least this wide
        // where video proxies (.sfvp0) are kept (empty: Reno's cache folder, or beside the media with proxyBeside)
        {"prefs/proxyFolder", QString()},
        {"prefs/proxyBeside", false},               // keep them beside the media, as VEGAS does, when proxyFolder is empty
        {"prefs/proxyCacheMb", 10240},  // the proxies in Reno's cache folder are cleared out (oldest first) above this size
        {"prefs/templateProject", QString()},       // File > New starts from this project (empty: an empty one)
        {"prefs/newAudioTrackHeight", 0},           // the height of new audio / video tracks in pixels (0: Reno's)
        {"prefs/newVideoTrackHeight", 0},
        {"prefs/defaultFadeInMs", 0},               // fades that events get when they are put on the timeline
        {"prefs/defaultFadeOutMs", 0},
        {"prefs/snapDistance", 8},                  // how near (pixels) something must come to snap
        {"prefs/scrubAudio", false},                // dragging the cursor on the ruler plays a short piece of sound
        {"prefs/scrubAudioMs", 100},                // ... this long
        {"prefs/scrubVolume", 100},                 // ... and this loud (per cent of what playing is)
        {"prefs/uiScale", 100},                     // the interface's size in per cent (read when Reno starts)
        {"prefs/timelineFontPt", 0},                // the timeline's text size in points (0: Reno's)
        // ask before leaving when the project has unsaved changes (off: never ask; the auto-save keeps the project)
        {"prefs/confirmExit", true},
        {"prefs/confirmCleanPrerender", false},     // ask before Clean Up Prerendered Video deletes the files
        {"prefs/confirmRemoveMedia", false},         // ask before removing media from the project
        {"prefs/warnMissingPlugins", false},         // say which plug-ins a project needs that are not installed, when it opens
        {"prefs/scanAtStartup", true},              // look for new and changed plug-in files when Reno starts
        {"prefs/renoCliPath", QString()},           // the reno_cli that checks plug-ins (empty: the one next to Reno)
    };
    return d;
}

namespace detail {
inline std::map<QString, QVariant>& cache()
{
    static std::map<QString, QVariant> c;
    return c;
}
}  // namespace detail

inline QVariant value(const QString& key)
{
    auto& c = detail::cache();
    auto it = c.find(key);
    if (it != c.end()) return it->second;
    const auto& d = defaults();
    const auto di = d.find(key);
    const QVariant v = QSettings().value(key, di != d.end() ? di->second : QVariant());
    c[key] = v;
    return v;
}
inline bool on(const QString& key) { return value(key).toBool(); }
inline int number(const QString& key) { return value(key).toInt(); }
inline double real(const QString& key) { return value(key).toDouble(); }
inline QString text(const QString& key) { return value(key).toString(); }
inline QStringList list(const QString& key) { return value(key).toStringList(); }
inline QColor color(const QString& key, const QColor& fallback = QColor())
{
    const QColor c(text(key));
    return c.isValid() ? c : fallback;
}
inline QVariant defaultOf(const QString& key)
{
    const auto& d = defaults();
    const auto di = d.find(key);
    return di != d.end() ? di->second : QVariant();
}
inline void set(const QString& key, const QVariant& v)
{
    QSettings().setValue(key, v);
    detail::cache()[key] = v;
}
inline void reload() { detail::cache().clear(); }

inline bool audioPluginShown(const std::string& path)
{
    const QStringList off = list("prefs/vstDisabled");
    return off.isEmpty() || !off.contains(QString::fromStdString(path));
}

// Preferences > Reno > Waveforms and images, "Thumbnail quality": the factor video thumbnails are requested at, against
// the size they are drawn at (draft half, normal 1, high twice).
inline double thumbScale()
{
    const QString q = text("prefs/thumbQuality");
    return q == "draft" ? 0.5 : q == "high" ? 2.0 : 1.0;
}

// Preferences > General, "Render large Wave files as Wave64" and "Allow Wave renders up to 4 GB": whether a .wav render
// of this many bytes goes to a Wave64 file (.w64) instead.
inline bool useWave64(qint64 bytes)
{
    return on("prefs/wave64") && bytes > (on("prefs/wave4gb") ? (qint64)4294967295LL : (qint64)2147483647LL);
}

// The curve a fade-type setting names, as Reno's envelope points and fades number them (project.h's Curve: 0 linear, 1
// fast, 2 slow, 3 smooth, 4 sharp, 5 hold).
inline int curveCode(const QString& key)
{
    const QString t = text(key);
    if (t == "fast") return 1;
    if (t == "slow") return 2;
    if (t == "smooth") return 3;
    if (t == "sharp") return 4;
    if (t == "hold") return 5;
    return 0;
}

// The same for keyframe types (project.h's KeyCurve: 0 linear, 1 hold, 2 smooth, 3 fast, 4 slow, 5 sharp).
inline int keyCurveCode(const QString& key)
{
    const QString t = text(key);
    if (t == "hold") return 1;
    if (t == "smooth") return 2;
    if (t == "fast") return 3;
    if (t == "slow") return 4;
    if (t == "sharp") return 5;
    return 0;
}

}  // namespace vprefs
}  // namespace app
}  // namespace reno
