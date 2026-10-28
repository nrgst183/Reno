// reno_cli: renders a project or media file from the command line with the same code as File > Render As.
// Also the plug-in checker (plugincheck.cpp): check, ofxprobe, audioprobe and dxprobe are picked by the first argument.
#include "../app/appstate.h"
#include "../app/prefsapply.h"
#include "../ofx/vkeffects.h"
#include "../app/builtintext.h"
#include "../app/logo.h"
#include "../app/rendertemplates.h"
#include "../core/process.h"
#include "../engine/catalog.h"
#include "../engine/engine.h"
#include "../engine/fxnames.h"
#include "../engine/profile.h"
#include "../engine/renderas.h"
#include "../engine/timefmt.h"
#include "../ofx/ofxhost.h"
#include "../vegas/vegas.h"
#include "plugincheck.h"

extern "C" {
#include <libavutil/log.h>
}

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <objbase.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

using namespace reno;
using namespace reno::app;

namespace {

// ------------------------------------------------------------------------------------------------ exit codes
enum Exit {
    kOk = 0,
    kRenderFailed = 1,
    kUsage = 2,             // bad command line, or --help
    kOpenFailed = 3,        // input could not be opened, or nothing to render
    kStrict = 4,            // --strict and media or plug-ins are missing
    kExists = 5,            // output exists and --overwrite was not given
    kCancelled = 130        // Ctrl+C
};

const char* kHelp =
    "reno_cli - render a VEGAS project, a Reno project or a media file from the command line, using the same\n"
    "code and templates as File > Render As. It also checks plug-ins (see below).\n"
    "\n"
    "Usage:\n"
    "  reno_cli [options] <project.veg | project.reno | media file>\n"
    "  reno_cli --list-templates [text]          list formats and templates (only those containing 'text')\n"
    "\n"
    "Output:\n"
    "  -o, --output FILE        output file (default: project name plus the format's extension, next to the\n"
    "                           project). The extension is added if missing. An image sequence writes\n"
    "                           FILE_000000.png, FILE_000001.png ...\n"
    "  -t, --template NAME      Render As template, \"Format/Template\". The format can be VEGAS's name or a short\n"
    "                           name: avc, hevc, wmv, avi, mov, mxf, xavcs, wav, w64, mp3, flac, ogg, m4a, ac3,\n"
    "                           wma, images, mkv, webm, ogv, opus, gif, apng, aiff, alac. The template name alone\n"
    "                           works if only one format has it. Custom templates are included. Default: the\n"
    "                           format for the output's extension (MP4: MAGIX AVC/AAC MP4), first template, at the\n"
    "                           project's frame size and rate.\n"
    "  -l, --list-templates [T] list templates and exit (-v: with what each one makes)\n"
    "  -y, --overwrite          replace the output file if it exists (otherwise reno_cli stops)\n"
    "\n"
    "Overriding the template:\n"
    "  --size WxH | project     frame size (e.g. 1280x720)\n"
    "  --width W, --height H    frame width or height (given alone, the other keeps the aspect ratio)\n"
    "  --fps N | N/D | project  frame rate (29.97 means 30000/1001; 23.976 and 59.94 likewise)\n"
    "  --video-codec C          h264, hevc, prores, dnxhd, mpeg2, mpeg4, mjpeg, vp9, vp8, av1, theora, gif, apng,\n"
    "                           qtrle, ffv1, utvideo, rawvideo, png, exr ... or an FFmpeg encoder name (libx264,\n"
    "                           h264_nvenc ...)\n"
    "  --video-bitrate R        average video bit rate: 8000, 8000k or 8M (Kbps unless k/M is given)\n"
    "  --max-bitrate R          maximum video bit rate (variable bit rate)\n"
    "  --cbr                    constant bit rate\n"
    "  --crf Q                  constant quality, 0 (best) to 51, instead of a bit rate\n"
    "  --two-pass               two passes (x264 / x265)\n"
    "  --encode-mode M          auto, cpu, nvenc, qsv, amf, mf (falls back if the encoder is not available)\n"
    "  --profile P, --level L   H.264 / HEVC profile and level (high, main, main10, 4.1 ...)\n"
    "  --audio-codec C          aac, mp3, pcm, flac, vorbis, opus, wma, ac3, alac or an FFmpeg encoder name\n"
    "  --audio-bitrate R        audio bit rate (Kbps; 192, 192k)\n"
    "  --sample-rate HZ | project\n"
    "  --channels 1|2|6         mono, stereo or 5.1\n"
    "  --audio-bits 16|24|32    PCM sample size (32 is floating point)\n"
    "  --quality Q              draft, preview, good or best: video rendering quality (default: the template's,\n"
    "                           else Project Properties'). --render-quality is the same.\n"
    "  --audio-only, --video-only\n"
    "\n"
    "Time range (default: start of the timeline to the end of the last event):\n"
    "  --range START END        same as --start START --end END\n"
    "  --start T, --end T, --length T\n"
    "                           T is seconds (12.5), hh:mm:ss.sss, hh:mm:ss;ff (timecode at the project's rate),\n"
    "                           or a frame count ending in f (300f)\n"
    "  --region NAME | #N       the project region with that name, or the N-th region (from 1)\n"
    "  --loop-region A,B        render only the loop region from A to B (like \"Render loop region only\" in\n"
    "                           Render As; .veg files do not store the loop region)\n"
    "\n"
    "Render As options:\n"
    "  --stretch                stretch the picture to the output frame (no letterbox)\n"
    "  --use-rotation           apply the project's output rotation\n"
    "  --markers                save markers and regions as chapters\n"
    "  --loudness-log           write <output>_loudness.txt (integrated loudness, range, true peak)\n"
    "  --summary                save the project summary (title, artist ...) as metadata\n"
    "  --project-path           save the project path in the file's comment\n"
    "\n"
    "Running:\n"
    "  --threads N              use at most N processors (effects, encoders, decoders); default all\n"
    "  --hw-decode auto|off     hardware decoding of H.264/HEVC/VP9/AV1/MPEG-2 video. Or name the API (d3d11va,\n"
    "                           dxva2, cuda, vaapi, vdpau, videotoolbox). Default: auto on Windows, off elsewhere.\n"
    "  --gpu on|off             render OpenFX effects with OpenGL where they support it (default on); off uses the\n"
    "                           processor only. Effects that fail with OpenGL fall back to the processor.\n"
    "  --vegas-fx real|builtin  VEGAS's own effects (Gaussian Blur, Cookie Cutter ...): Reno's built-in versions,\n"
    "                           ignoring the installed ones (builtin; default, or the Preferences > Video setting),\n"
    "                           or VEGAS's OpenFX plug-ins if installed and working (real)\n"
    "  --dx-fx real|builtin     VEGAS's DirectX audio effects (Reverb, EQ, Track FX ...): Reno's versions (builtin;\n"
    "                           default, or the Preferences setting) or the installed plug-ins on Windows (real)\n"
    "  --efx real|builtin       VEGAS's eFX audio effects (Limiter, Compressor, Reverb, TubeStage, Phaser,\n"
    "                           ChorusFlanger): Reno's own versions (builtin; default, or the Preferences setting)\n"
    "                           or the installed MAGIX plug-ins (real; they need a VEGAS licence)\n"
    "  --proxies                with --profile: use the media's video proxies where they exist\n"
    "  --hash [FILE]            with --profile: print one hash over all rendered pictures (and write each\n"
    "                           picture's hash to FILE), to check that an engine change leaves them unchanged\n"
    "  --full-size-fx           with --profile: run effects without multi-resolution support at the project size\n"
    "                           (default: the preview size, as VEGAS does)\n"
    "  --no-fx                  bypass every effect (events, tracks, buses, master, video output; not inside nested\n"
    "                           projects), for timing comparisons\n"
    "  --strict                 stop before rendering if any media file or plug-in is missing (without it they are\n"
    "                           listed and the render goes on without them)\n"
    "  --no-scan                do not look for new or changed plug-in files (use the saved plug-in list)\n"
    "  --plugin-path DIR        also look for OpenFX, VST and VST3 plug-ins in DIR (this run only; repeatable)\n"
    "  -q, --quiet              only errors and the result\n"
    "  -v, --verbose            more output: loaded plug-ins, differences from VEGAS, Reno's log\n"
    "  --log FILE               write Reno's log to FILE\n"
    "  --version, -h / --help\n"
    "\n"
    "Profiling the preview:\n"
    "  reno_cli vkcheck [--only TEXT] [--size WxH] [--repeat N] [--device N] [--software] [--report FILE]\n"
    "                           list the Vulkan devices and render every built-in effect with GPU kernels on the\n"
    "                           CPU and the GPU at several settings, reporting the largest and mean difference and\n"
    "                           the times. Writes vkcheck_report.txt. --profile also takes --gpu-api\n"
    "                           vulkan|opengl|off to compare speeds.\n"
    "  reno_cli --profile <project.veg> [--range START END] [--scale 0.5] [--threads N] [--gpu on|off]\n"
    "                           render the range frame by frame like the playing preview, at --scale of the project\n"
    "                           size (default 0.5, default range: the first 10 s). Prints the average and worst\n"
    "                           frame time, the frame rate, and a table of where the time went: per plug-in (total\n"
    "                           ms, calls, ms per call, preview or project size, thread safety), media decoding\n"
    "                           and seeks, and the rest. Writes nothing. --threads N renders N pictures at once\n"
    "                           (default 1, and 1 if a plug-in is not thread safe) and limits processors used.\n"
    "                           (--profile followed by something that is not a project file is the H.264 profile.)\n"
    "                           --preview-profile is the same as --profile with a project.\n"
    "\n"
    "Single pictures (for comparing with VEGAS snapshots, see tools/fuzz_compare.py):\n"
    "  reno_cli --stills TIMES --stills-dir DIR <project.veg>\n"
    "                           render the picture at each time as a PNG, at final render quality and project size.\n"
    "                           TIMES: comma-separated seconds, or a text file with one time (or name=time) per\n"
    "                           line or separated by commas/blanks. With name=T the file is DIR/name.png\n"
    "                           (default still_0000.png ...).\n"
    "  --find-media             look for missing media by file name next to the project (for --stills, --profile\n"
    "                           and --audiofx-report)\n"
    "\n"
    "Plug-ins: OpenFX, VST2, VST3 and DirectX effects are found the way the editor finds them: the saved plug-in\n"
    "list (Options > Look for New Plug-Ins), Reno's plug-in folders, VEGAS's folders, and OFX_PLUGIN_PATH. New or\n"
    "changed plug-in files are tried first, each in its own reno_cli process, unless --no-scan. Nested projects\n"
    "(.veg files used as media) are rendered with their effects.\n"
    "\n"
    "Checking plug-ins:\n"
    "  reno_cli --vst-params <plug-in file or folder>\n"
    "                           set every parameter of each VST 2 plug-in (every .dll / .so in a folder) from 0 to\n"
    "                           1 in steps of 1/40 and record the value text; writes vst_params.txt\n"
    "  reno_cli --audiofx-report <project.veg>\n"
    "                           for every audio effect in the project and its nested projects: its settings, whether\n"
    "                           the saved settings were applied, and the gain it gives a test tone and noise;\n"
    "                           writes audiofx_report.txt\n"
    "  reno_cli check [--out-dir DIR] [--no-video] [--no-audio] [--no-dx]\n"
    "                           try every OpenFX, VST and DirectX plug-in on this machine, each in its own process;\n"
    "                           writes plugin_report.txt and plugin_report.json (in DIR, default: current folder)\n"
    "  reno_cli vegasfx [--out-dir DIR] [--size WxH] [--frames N] [--only TEXT] [--plugin-path DIR] [--quick]\n"
    "                   [--max-params N] [--gpu on|off] [--stubs] [--all] [--timeout SECONDS]\n"
    "                   [--resources DIR] [--params-only] [--keep-real]\n"
    "                           for every VEGAS effect, try VEGAS's plug-in (if installed) and Reno's built-in\n"
    "                           version, each in its own process, on a test picture: defaults, each setting at its\n"
    "                           minimum / middle / maximum, and the presets. Writes vegasfx_report.txt and .json\n"
    "                           (which load, crash, fail or draw nothing; time per picture; how far the built-in is\n"
    "                           from the real one) and side-by-side pictures of the worst cases into DIR.\n"
    "                           --size: test picture size (default 320x180). --frames: frames at the defaults (3).\n"
    "                           --only: part of an effect's name or identifier. --max-params: settings tried one at\n"
    "                           a time (12). --quick: only the first four presets. --all: also non-VEGAS plug-ins.\n"
    "                           Without VEGAS's plug-ins installed only the built-ins are tried. The report also\n"
    "                           compares each built-in's settings (names, types, choices, labels) with the resource\n"
    "                           files (Contents\\Resources\\*.xml) of VEGAS's OFX bundles, found with the plug-ins\n"
    "                           or in --resources DIR. --params-only does only that (vegasfx_params.txt and .json).\n"
    "\n"
    "Progress, messages and errors go to stderr; stdout gets only the result (\"Rendered <file> ...\").\n"
    "Ctrl+C stops the render and deletes the partial file.\n"
    "\n"
    "Exit codes: 0 rendered; 1 render failed; 2 bad command line; 3 input could not be opened or nothing to render;\n"
    "4 --strict and something is missing; 5 output exists (use -y); 130 stopped with Ctrl+C.\n"
    "\n"
    "Examples:\n"
    "  reno_cli project.veg\n"
    "      writes project.mp4 next to it: H.264/AAC at the project's size and frame rate\n"
    "  reno_cli project.veg -o out.mp4 -t \"MAGIX AVC/AAC MP4/Internet HD 1080p 29.97 fps\" -y\n"
    "  reno_cli project.veg -o out.mp4 -t \"avc/Internet HD 720p 25 fps\" --video-bitrate 6M --two-pass\n"
    "  reno_cli project.veg -o out.mov -t \"QuickTime 7/Apple ProRes 422 HQ\" --start 10 --end 00:00:20.000\n"
    "  reno_cli project.veg -o song.wav --audio-only --region \"Chorus\"\n"
    "  reno_cli project.veg -o song.mp3 -t \"MP3 Audio/320 Kbps, CD Transparent Audio\"\n"
    "  reno_cli project.veg -o frames/shot.png -t \"Image Sequence/PNG\" --loop-region 5,6\n"
    "  reno_cli clip.mov -o clip.mp4 --size 1280x720 --fps 30 --crf 20\n"
    "  reno_cli project.veg -o out.mkv -t \"Matroska (MKV)/Project size and rate\" --video-codec hevc --threads 4\n"
    "  reno_cli --list-templates 1080p -v\n";

// ------------------------------------------------------------------------------------------------ the console
std::atomic<bool> gCancel{false};
std::atomic<int> gInterrupts{0};
bool gQuiet = false, gVerbose = false;
bool gTty = false;
bool gProgressShown = false;            // a progress line without a newline is on screen
std::mutex gOutMutex;
std::deque<std::string> gLogTail;       // last log lines, shown on failure
std::FILE* gLogFile = nullptr;

// Everything except the result, --help and --list-templates goes to stderr; stdout only gets "Rendered <file>".
void endProgressLine()
{
    if (gProgressShown) {
        std::fputs("\n", stderr);
        gProgressShown = false;
    }
}

void out(const std::string& s)
{
    std::lock_guard<std::mutex> lk(gOutMutex);
    if (gQuiet) return;
    endProgressLine();
    std::fputs(s.c_str(), stderr);
    std::fputs("\n", stderr);
    std::fflush(stderr);
}

void err(const std::string& s)
{
    std::lock_guard<std::mutex> lk(gOutMutex);
    endProgressLine();
    std::fflush(stdout);
    std::fputs(("reno_cli: " + s + "\n").c_str(), stderr);
    std::fflush(stderr);
}

void progressLine(const std::string& s)
{
    std::lock_guard<std::mutex> lk(gOutMutex);
    if (gQuiet) return;
    if (gTty) {
        // redraw in place, padded to cover a longer previous line
        std::string line = s;
        if (line.size() < 79) line.append(79 - line.size(), ' ');
        std::fputs(("\r" + line).c_str(), stderr);
        gProgressShown = true;
    } else {
        std::fputs((s + "\n").c_str(), stderr);
    }
    std::fflush(stderr);
}

#ifdef _WIN32
BOOL WINAPI consoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (gInterrupts.fetch_add(1) >= 1) ExitProcess(kCancelled);       // second Ctrl+C quits at once
        gCancel.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
void onSignal(int)
{
    if (gInterrupts.fetch_add(1) >= 1) _exit(kCancelled);                 // second Ctrl+C quits at once
    gCancel.store(true);
}
#endif

#ifdef _WIN32
// OpenFX plug-ins get a host window (kOfxPropHostOSHandle) and some create a device on it or parent dialogs to it.
// Here it is a hidden window; Qt's event loop runs between frames to pump its messages.
HWND makeHiddenHostWindow()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RenoRenderHostWindow";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"reno_cli", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, nullptr, nullptr,
                           wc.hInstance, nullptr);
}
#endif

std::string clockText(double seconds)
{
    const long long s = (long long)std::max(0.0, seconds + 0.5);
    if (s >= 3600) return strfmt("%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    return strfmt("%lld:%02lld", s / 60, s % 60);
}

std::string sizeText(long long bytes)
{
    if (bytes < 1024) return strfmt("%lld bytes", bytes);
    if (bytes < 1024 * 1024) return strfmt("%.1f KB", bytes / 1024.0);
    if (bytes < 1024LL * 1024 * 1024) return strfmt("%.1f MB", bytes / (1024.0 * 1024.0));
    return strfmt("%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

std::string s(const QString& q) { return q.toStdString(); }
QString q(const std::string& v) { return QString::fromStdString(v); }

// ------------------------------------------------------------------------------------------------ reading values
// "8M", "8000k", "8000" -> Kbps
bool parseKbps(const std::string& text, int* kbps)
{
    if (text.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    std::string unit = toLower(std::string(end ? end : ""));
    if (unit == "bps" || unit == "kbps" || unit == "kb/s") unit = unit == "bps" ? "b" : "k";
    if (unit == "mbps" || unit == "mb/s") unit = "m";
    double k = v;
    if (unit == "m") k = v * 1000.0;
    else if (unit == "b") k = v / 1000.0;
    else if (!unit.empty() && unit != "k") return false;
    if (!(k >= 8 && k <= 2000000)) return false;
    *kbps = (int)std::lround(k);
    return true;
}

bool parseInt(const std::string& text, int lo, int hi, int* v)
{
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || (end && *end) || n < lo || n > hi) return false;
    *v = (int)n;
    return true;
}

// 29.97 -> 30000/1001 (same for the other NTSC rates)
bool parseFps(const std::string& text, int* num, int* den)
{
    const size_t slash = text.find('/');
    if (slash != std::string::npos) {
        int a = 0, b = 0;
        if (!parseInt(text.substr(0, slash), 1, 1000000, &a) || !parseInt(text.substr(slash + 1), 1, 1000000, &b)) return false;
        *num = a;
        *den = b;
        return (double)a / b <= 1000.0;
    }
    char* end = nullptr;
    const double f = std::strtod(text.c_str(), &end);
    if (text.empty() || (end && *end) || !(f > 0 && f <= 1000)) return false;
    for (int base : {24, 30, 48, 60, 120, 240}) {
        if (std::fabs(f - base * 1000.0 / 1001.0) < 0.006) {
            *num = base * 1000;
            *den = 1001;
            return true;
        }
    }
    if (std::fabs(f - std::round(f)) < 1e-6) {
        *num = (int)std::lround(f);
        *den = 1;
        return true;
    }
    *num = (int)std::lround(f * 1000.0);
    *den = 1000;
    return true;
}

void fpsFraction(double fps, int* num, int* den)
{
    if (!parseFps(strfmt("%.6f", fps), num, den)) {
        *num = 30;
        *den = 1;
    }
}

// Seconds, hh:mm:ss.sss, hh:mm:ss;ff (at the project's rate) or 300f (frames).
bool parseTime(const ProjectSettings& ps, const std::string& text, double* seconds)
{
    if (text.empty()) return false;
    char* end = nullptr;
    if (text.back() == 'f' || text.back() == 'F') {
        const double frames = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() - 1 || frames < 0) return false;
        *seconds = frames / ps.fps();
        return true;
    }
    const double v = std::strtod(text.c_str(), &end);
    if (end && *end == 0) {
        if (v < 0) return false;
        *seconds = v;
        return true;
    }
    // typed times count from the ruler's start
    double shown = 0;
    const std::string format = text.find(';') != std::string::npos ? "timecode" : "time";
    if (!parseTimeText(ps, text, format, &shown) && !parseTimeText(ps, text, "timecode", &shown)) return false;
    *seconds = std::max(0.0, shown - ps.rulerStart);
    return true;
}

// ------------------------------------------------------------------------------------------------ what is missing
// Looks up a nested project's audio plug-in as FxRuntime::prepareAudio does, without starting it.
bool audioPluginThere(const AudioPluginDesc& d0, std::string* why)
{
    AudioPluginDesc d = d0;
    if (d.format == "BUILTIN") return true;
    if (d.format == "VST2" && efxPreferBuiltin() && efxReproducedId(d.uid)) return true;      // Reno's own eFX effects need no file
    const std::vector<AudioFxEntry> all = Catalog::get().audio();
    if (d.format == "DX") {
        for (const auto& e : all)
            if (e.desc.format == "DX" && toLower(e.desc.uid) == toLower(d.uid)) return true;
        static std::vector<AudioPluginDesc> dx = listDirectXAudioPlugins();
        for (const auto& e : dx)
            if (toLower(e.uid) == toLower(d.uid)) return true;
        const std::string hint = audioPluginFileHint(d);
        *why = "this DirectX plug-in is not installed" + (hint.empty() ? std::string() : " (its file is " + hint + ")");
        return false;
    }
    if (d.path.empty() || !fileExists(d.path)) {
        d.path.clear();
        for (const auto& e : all)
            if (e.desc.format == d.format && e.desc.uid == d.uid) { d.path = e.desc.path; break; }
        if (d.path.empty())
            for (const auto& e : all)
                if (e.desc.format == d.format && toLower(e.desc.name) == toLower(d.name)) { d.path = e.desc.path; break; }
    }
    if (d.path.empty()) {
        const std::string hint = audioPluginFileHint(d);
        *why = "is not installed: Reno found no " + d.format + " plug-in " +
               (d.format == "VST2" ? "with the id '" + vstIdText(d.uid) + "'" : "with the class id " + d.uid) + " in the plug-in folders it looks in" +
               (hint.empty() ? std::string() : " (the file it needs is " + hint + ")");
        return false;
    }
    if (Catalog::get().crashedWhenTried(d.path)) {
        *why = pathFileName(d.path) + " crashed when Reno tried it";
        return false;
    }
    return true;
}

struct Missing {
    std::vector<std::string> media;         // "file  (in nested.veg)"
    std::vector<std::string> plugins;       // "Name (kind): why  [in nested.veg]"
};

std::string slotKind(const FxSlot& sl)
{
    if (sl.kind == "ofx") return "OpenFX";
    if (sl.audio.format == "DX") return "DirectX";
    if (sl.audio.format == "VST2") return "VST";
    return sl.audio.format.empty() ? std::string("audio") : sl.audio.format;
}

std::string slotName(const FxSlot& sl)
{
    // the effect's label as the editor shows it, then (OpenFX) its identifier
    std::string n = fxSlotLabel(sl);
    if (sl.kind == "ofx" && !sl.pluginId.empty() && n != sl.pluginId) n += " [" + sl.pluginId + "]";
    return n + " (" + slotKind(sl) + ")";
}

// Missing media and plug-ins of nested projects (each project once).
void checkNested(const Project& p, const std::string& where, std::set<std::string>& seen, Missing& m, int depth)
{
    if (depth > 16) return;
    for (const MediaItem& mi : p.media) {
        if (mi.generated || mi.path.empty() || mi.path.compare(0, 6, "synth:") == 0) continue;
        if (!isProjectFile(mi.path) || !fileExists(mi.path)) continue;
        const std::string key = toLower(mi.path);
        if (seen.count(key)) continue;
        seen.insert(key);
        Project n;
        std::string e;
        if (!loadProjectFile(mi.path, n, &e)) {
            m.media.push_back(mi.path + "  (a nested project that could not be read: " + e + ")");
            continue;
        }
        const std::string in = "  [in " + pathFileName(mi.path) + "]";
        for (const MediaItem& x : n.media)
            if (!x.generated && !x.path.empty() && x.path.compare(0, 6, "synth:") != 0 && !fileExists(x.readPath())) m.media.push_back(x.path + in);
        n.forEachFx([&](const FxSlot& sl) {
            std::string why;
            if (sl.kind == "ofx") {
                if (!findVideoPlugin(sl.pluginId, &why)) m.plugins.push_back(slotName(sl) + ": " + (why.empty() ? "not found" : why) + in);
            } else if (sl.kind == "audio") {
                if (!audioPluginThere(sl.audio, &why)) m.plugins.push_back(slotName(sl) + ": " + why + in);
            }
        });
        checkNested(n, mi.path, seen, m, depth + 1);
    }
    (void)where;
}

// Unique lines in first-seen order, with a count: "line  (x12)".
std::vector<std::string> counted(const std::vector<std::string>& lines)
{
    std::vector<std::string> order;
    std::map<std::string, int> n;
    for (const std::string& l : lines)
        if (n[l]++ == 0) order.push_back(l);
    for (std::string& l : order)
        if (n[l] > 1) l += strfmt("  (x%d)", n[l]);
    return order;
}

// ------------------------------------------------------------------------------------------------ the options
struct Options {
    std::string input, output, templ, listFilter, logFile;
    bool list = false, overwrite = false, strict = false, noScan = false, noFx = false;
    bool audioOnly = false, videoOnly = false;
    // overrides
    bool sizeProject = false, fpsProject = false, rateProject = false;
    int width = 0, height = 0, onlyWidth = 0, onlyHeight = 0, fpsNum = 0, fpsDen = 0, sampleRate = 0, channels = 0, bits = 0;
    int kbps = 0, maxKbps = 0, audioKbps = 0, crf = -1;
    bool cbr = false, twoPass = false;
    std::string videoCodec, audioCodec, encodeMode, profile, level, renderQuality;
    // range
    std::string start, end, length, region, loopRegion;
    // Render As's options
    bool stretch = false, rotation = false, markers = false, loudness = false, summary = false, projectPath = false;
    int threads = 0;
    int gpu = -1;                           // --gpu on|off (-1: not given)
    bool fullSizeFx = false;                // --full-size-fx
    bool proxies = false;                   // --proxies
    bool hashFrames = false;                // --hash [FILE]
    std::string hashFile;
    int efx = -1;                           // --efx real|builtin (-1: use the preference)
    int vegasFx = -1;                       // --vegas-fx real|builtin (-1: use the preference)
    int dxFx = -1;                          // --dx-fx real|builtin (-1: use the preference)
    std::string gpuApi;                     // --gpu-api vulkan|opengl|off (empty: use the preference)
    bool previewProfile = false;            // --profile <project>
    std::string vstParams;                  // --vst-params <plug-in file or folder>
    bool audioFxReport = false;             // --audiofx-report <project>
    std::string stills, stillsDir;          // --stills TIMES --stills-dir DIR
    bool findMedia = false;                 // --find-media
    double previewScale = 0.5;              // --scale
    std::string hwDecode;                   // --hw-decode: auto, off or an API name (empty: auto on Windows, off elsewhere)
    std::vector<std::string> pluginPaths;   // --plugin-path
};

const std::set<std::string> kVideoFamilies = {"h264", "hevc", "prores", "dnxhd", "mpeg2", "dv", "mpeg4", "mjpeg", "wmv", "vp9",
                                              "rawvideo", "utvideo", "png", "jpeg", "tiff", "bmp",
                                              "vp8", "av1", "theora", "gif", "apng", "qtrle", "ffv1", "exr", "tga", "webp"};
const std::set<std::string> kAudioFamilies = {"aac", "mp3", "pcm", "flac", "vorbis", "opus", "wma", "ac3", "alac"};

// True if a --profile value is a project (otherwise it is an H.264 profile such as high or main).
static bool looksLikeProject(const std::string& v)
{
    const std::string l = toLower(v);
    auto ends = [&](const char* e) { const size_t n = std::strlen(e); return l.size() > n && l.compare(l.size() - n, n, e) == 0; };
    return ends(".veg") || ends(".reno") || ends(".vf") || l.find_first_of("/\\") != std::string::npos;
}

// Parses the command line; returns false with a message in *problem on bad input.
bool parseArgs(const std::vector<std::string>& args, Options& o, std::string* problem, bool* help, bool* version)
{
    auto need = [&](size_t& k, const std::string& name, std::string* v) {
        if (k + 1 >= args.size()) {
            *problem = name + " needs a value";
            return false;
        }
        *v = args[++k];
        return true;
    };
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        std::string v;
        if (a == "-h" || a == "--help" || a == "/?") { *help = true; return true; }
        else if (a == "--version") { *version = true; return true; }
        else if (a == "-o" || a == "--output") { if (!need(k, a, &o.output)) return false; }
        else if (a == "-t" || a == "--template") { if (!need(k, a, &o.templ)) return false; }
        else if (a == "-l" || a == "--list-templates") {
            o.list = true;
            if (k + 1 < args.size() && args[k + 1].compare(0, 1, "-") != 0) o.listFilter = args[++k];
        }
        else if (a == "-y" || a == "--overwrite") o.overwrite = true;
        else if (a == "--strict") o.strict = true;
        else if (a == "--no-fx") o.noFx = true;
        else if (a == "--no-scan") o.noScan = true;
        else if (a == "-q" || a == "--quiet") gQuiet = true;
        else if (a == "-v" || a == "--verbose") gVerbose = true;
        else if (a == "--log") { if (!need(k, a, &o.logFile)) return false; }
        else if (a == "--audio-only") o.audioOnly = true;
        else if (a == "--video-only") o.videoOnly = true;
        else if (a == "--size") {
            if (!need(k, a, &v)) return false;
            if (toLower(v) == "project") o.sizeProject = true;
            else {
                const size_t x = toLower(v).find('x');
                if (x == std::string::npos || !parseInt(v.substr(0, x), 16, 16384, &o.width) || !parseInt(v.substr(x + 1), 16, 16384, &o.height)) {
                    *problem = "--size wants WIDTHxHEIGHT (16 to 16384), e.g. 1920x1080, or 'project'";
                    return false;
                }
            }
        }
        else if (a == "--width" || a == "--height") {
            if (!need(k, a, &v)) return false;
            int n = 0;
            if (!parseInt(v, 16, 16384, &n)) { *problem = a + " wants 16 to 16384 pixels"; return false; }
            (a == "--width" ? o.onlyWidth : o.onlyHeight) = n;
        }
        else if (a == "--range") {
            if (k + 2 >= args.size()) { *problem = "--range needs two times: START END"; return false; }
            o.start = args[++k];
            o.end = args[++k];
        }
        else if (a == "--plugin-path") {
            if (!need(k, a, &v)) return false;
            o.pluginPaths.push_back(v);
        }
        else if (a == "--fps") {
            if (!need(k, a, &v)) return false;
            if (toLower(v) == "project") o.fpsProject = true;
            else if (!parseFps(v, &o.fpsNum, &o.fpsDen)) { *problem = "--fps wants a frame rate: 25, 29.97, 30000/1001 or 'project'"; return false; }
        }
        else if (a == "--sample-rate") {
            if (!need(k, a, &v)) return false;
            if (toLower(v) == "project") o.rateProject = true;
            else if (!parseInt(v, 8000, 384000, &o.sampleRate)) { *problem = "--sample-rate wants 8000 to 384000 or 'project'"; return false; }
        }
        else if (a == "--channels") {
            if (!need(k, a, &v)) return false;
            if (!parseInt(v, 1, 6, &o.channels) || (o.channels != 1 && o.channels != 2 && o.channels != 6)) { *problem = "--channels wants 1, 2 or 6"; return false; }
        }
        else if (a == "--audio-bits") {
            if (!need(k, a, &v)) return false;
            if (!parseInt(v, 16, 32, &o.bits) || (o.bits != 16 && o.bits != 24 && o.bits != 32)) { *problem = "--audio-bits wants 16, 24 or 32"; return false; }
        }
        else if (a == "--video-bitrate" || a == "--vb") {
            if (!need(k, a, &v)) return false;
            if (!parseKbps(v, &o.kbps)) { *problem = a + " wants a bit rate: 8000 (Kbps), 8000k or 8M"; return false; }
        }
        else if (a == "--max-bitrate") {
            if (!need(k, a, &v)) return false;
            if (!parseKbps(v, &o.maxKbps)) { *problem = "--max-bitrate wants a bit rate: 12000 (Kbps), 12000k or 12M"; return false; }
        }
        else if (a == "--audio-bitrate" || a == "--ab") {
            if (!need(k, a, &v)) return false;
            if (!parseKbps(v, &o.audioKbps) || o.audioKbps > 1536) { *problem = a + " wants a bit rate of 8 to 1536 Kbps"; return false; }
        }
        else if (a == "--crf") {
            if (!need(k, a, &v)) return false;
            if (!parseInt(v, 0, 51, &o.crf)) { *problem = "--crf wants 0 to 51"; return false; }
        }
        else if (a == "--cbr") o.cbr = true;
        else if (a == "--two-pass") o.twoPass = true;
        else if (a == "--video-codec" || a == "--vcodec") { if (!need(k, a, &o.videoCodec)) return false; }
        else if (a == "--audio-codec" || a == "--acodec") { if (!need(k, a, &o.audioCodec)) return false; }
        else if (a == "--encode-mode") {
            if (!need(k, a, &o.encodeMode)) return false;
            o.encodeMode = toLower(o.encodeMode);
            if (o.encodeMode != "auto" && o.encodeMode != "cpu" && o.encodeMode != "nvenc" && o.encodeMode != "qsv" && o.encodeMode != "amf" && o.encodeMode != "mf") {
                *problem = "--encode-mode wants auto, cpu, nvenc, qsv, amf or mf";
                return false;
            }
        }
        else if (a == "--preview-profile" || (a == "--profile" && k + 1 < args.size() && looksLikeProject(args[k + 1]))) {
            if (!need(k, a, &v)) return false;
            o.previewProfile = true;
            if (!o.input.empty()) { *problem = "more than one input file: " + o.input + " and " + v; return false; }
            o.input = v;
        }
        else if (a == "--vst-params") {
            if (!need(k, a, &o.vstParams)) return false;
        }
        else if (a == "--audiofx-report") {
            if (!need(k, a, &v)) return false;
            o.previewProfile = true;
            o.audioFxReport = true;
            if (!o.input.empty()) { *problem = "more than one input file: " + o.input + " and " + v; return false; }
            o.input = v;
        }
        else if (a == "--stills") {
            if (!need(k, a, &o.stills)) return false;
            o.previewProfile = true;
        }
        else if (a == "--stills-dir") { if (!need(k, a, &o.stillsDir)) return false; }
        else if (a == "--find-media") o.findMedia = true;
        else if (a == "--scale") {
            if (!need(k, a, &v)) return false;
            char* endp = nullptr;
            o.previewScale = std::strtod(v.c_str(), &endp);
            if (!endp || *endp || !(o.previewScale >= 0.0625 && o.previewScale <= 1.0)) { *problem = "--scale wants 0.0625 to 1 (the fraction of the project's size)"; return false; }
        }
        else if (a == "--profile") { if (!need(k, a, &o.profile)) return false; }
        else if (a == "--level") { if (!need(k, a, &o.level)) return false; }
        else if (a == "--render-quality" || a == "--quality") {
            if (!need(k, a, &o.renderQuality)) return false;
            o.renderQuality = toLower(o.renderQuality);
            if (o.renderQuality != "draft" && o.renderQuality != "preview" && o.renderQuality != "good" && o.renderQuality != "best") {
                *problem = "--render-quality wants draft, preview, good or best";
                return false;
            }
        }
        else if (a == "--start") { if (!need(k, a, &o.start)) return false; }
        else if (a == "--end") { if (!need(k, a, &o.end)) return false; }
        else if (a == "--length") { if (!need(k, a, &o.length)) return false; }
        else if (a == "--region") { if (!need(k, a, &o.region)) return false; }
        else if (a == "--loop-region" || a == "--selection") { if (!need(k, a, &o.loopRegion)) return false; }
        else if (a == "--stretch") o.stretch = true;
        else if (a == "--use-rotation") o.rotation = true;
        else if (a == "--markers") o.markers = true;
        else if (a == "--loudness-log") o.loudness = true;
        else if (a == "--summary") o.summary = true;
        else if (a == "--project-path") o.projectPath = true;
        else if (a == "--gpu") {
            if (!need(k, a, &v)) return false;
            for (char& c : v) c = (char)std::tolower((unsigned char)c);
            if (v != "on" && v != "off") { *problem = "--gpu wants on or off"; return false; }
            o.gpu = v == "on" ? 1 : 0;
        }
        else if (a == "--gpu-api") {
            if (!need(k, a, &v)) return false;
            for (char& c : v) c = (char)std::tolower((unsigned char)c);
            if (v != "vulkan" && v != "opengl" && v != "off") { *problem = "--gpu-api wants vulkan, opengl or off"; return false; }
            o.gpuApi = v;
        }
        else if (a == "--vegas-fx") {
            if (!need(k, a, &v)) return false;
            for (char& c : v) c = (char)std::tolower((unsigned char)c);
            if (v == "real" || v == "vegas" || v == "own") o.vegasFx = 0;
            else if (v == "builtin" || v == "built-in" || v == "reno") o.vegasFx = 1;
            else { *problem = "--vegas-fx wants real or builtin"; return false; }
        }
        else if (a == "--dx-fx") {
            if (!need(k, a, &v)) return false;
            for (char& c : v) c = (char)std::tolower((unsigned char)c);
            if (v == "real" || v == "vegas" || v == "installed") o.dxFx = 0;
            else if (v == "builtin" || v == "built-in" || v == "reno") o.dxFx = 1;
            else { *problem = "--dx-fx wants real or builtin"; return false; }
        }
        else if (a == "--efx") {
            if (!need(k, a, &v)) return false;
            for (char& c : v) c = (char)std::tolower((unsigned char)c);
            if (v == "real" || v == "vegas" || v == "installed") o.efx = 0;
            else if (v == "builtin" || v == "built-in" || v == "reno") o.efx = 1;
            else { *problem = "--efx wants real or builtin"; return false; }
        }
        else if (a == "--full-size-fx") o.fullSizeFx = true;
        else if (a == "--proxies") o.proxies = true;
        else if (a == "--hash") {
            o.hashFrames = true;
            if (k + 1 < args.size() && args[k + 1].compare(0, 2, "--") != 0) o.hashFile = args[++k];
        }
        else if (a == "--hw-decode") {
            if (!need(k, a, &v)) return false;
            for (char& c : v) c = (char)std::tolower((unsigned char)c);
            bool known = v == "auto" || v == "off";
            for (const std::string& api : hardwareDecodeApis()) known = known || v == api;
            if (!known) {
                std::string list;
                for (const std::string& api : hardwareDecodeApis()) list += ", " + api;
                *problem = "--hw-decode wants auto, off" + list;
                return false;
            }
            o.hwDecode = v;
        }
        else if (a == "--threads") {
            if (!need(k, a, &v)) return false;
            if (!parseInt(v, 1, 256, &o.threads)) { *problem = "--threads wants 1 to 256"; return false; }
        }
        else if (a.size() > 1 && a[0] == '-') { *problem = "unknown option " + a + " (reno_cli --help lists them)"; return false; }
        else if (o.input.empty()) o.input = a;
        else { *problem = "more than one input file: " + o.input + " and " + a; return false; }
    }
    if (o.audioOnly && o.videoOnly) { *problem = "--audio-only and --video-only together leave nothing to render"; return false; }
    if (o.crf >= 0 && (o.cbr || o.kbps)) { *problem = "--crf (constant quality) cannot go with --cbr or --video-bitrate"; return false; }
    if (o.width > 0 && (o.onlyWidth || o.onlyHeight)) { *problem = "--size and --width/--height together: give one or the other"; return false; }
    if (!o.list && o.input.empty() && o.vstParams.empty()) { *problem = "no project or media file given (reno_cli --help)"; return false; }
    return true;
}

// ------------------------------------------------------------------------------------------------ templates
// The format whose files have this extension (mp4: MAGIX AVC/AAC MP4). -1 if none.
int formatForExtension(const std::vector<rt::Format>& all, const std::string& ext)
{
    const QString e = q(toLower(ext));
    if (e.isEmpty()) return -1;
    if (e == "mp4") {
        for (size_t k = 0; k < all.size(); ++k) if (all[k].key == "avc") return (int)k;
    }
    for (size_t k = 0; k < all.size(); ++k)
        if (all[k].ext == e) return (int)k;
    if (e == "aiff" || e == "aifc") return formatForExtension(all, "aif");
    if (e == "oga") return formatForExtension(all, "ogg");
    if (e == "jpg" || e == "jpeg" || e == "tif" || e == "tiff" || e == "bmp" || e == "exr" || e == "tga" || e == "webp")
        for (size_t k = 0; k < all.size(); ++k) if (all[k].sequence) return (int)k;
    return -1;
}

int listTemplates(const std::string& filter)
{
    const std::vector<rt::Format> all = rt::loadFormats();
    const QString f = q(filter);
    int n = 0;
    for (const rt::Format& fmt : all) {
        bool header = false;
        for (const rt::Tmpl& t : fmt.templates) {
            const QString full = fmt.name + "/" + t.name;
            if (!f.isEmpty() && !full.contains(f, Qt::CaseInsensitive) && !fmt.key.contains(f, Qt::CaseInsensitive)) continue;
            if (!header) {
                std::printf("%s  [%s, .%s]%s\n", s(fmt.name).c_str(), s(fmt.key).c_str(), s(fmt.ext).c_str(),
                            gVerbose ? ("  " + s(fmt.blurb)).c_str() : "");
                header = true;
            }
            std::printf("    %s%s\n", s(t.name).c_str(), t.custom ? "  (custom)" : "");
            if (gVerbose) {
                for (const QString& line : rt::templateInfo(fmt, t).split('\n')) std::printf("        %s\n", s(line).c_str());
            }
            ++n;
        }
    }
    if (n == 0) {
        std::fprintf(stderr, "reno_cli: no template's name contains \"%s\"\n", filter.c_str());
        return kUsage;
    }
    std::printf("\n%d template%s. Give one with -t \"Format/Template\" (or [short name]/Template).\n", n, n == 1 ? "" : "s");
    return kOk;
}

// ------------------------------------------------------------------------------------------------ opening
// A single media file as a project: settings matched to the file, placed at 0 on a video or audio track.
bool openMediaFile(const std::string& path, std::string* error)
{
    AppState& app = AppState::get();
    std::string e;
    std::unique_ptr<MediaReader> r = app.mediaOpener(path, &e);
    if (!r) {
        *error = e.empty() ? "it is not a media file Reno can read" : e;
        return false;
    }
    const MediaInfo info = r->info();
    r.reset();
    if (!info.hasVideo && !info.hasAudio) {
        *error = "it has neither pictures nor sound";
        return false;
    }
    app.newProject(true);
    app.edit("Match Media Settings", [&](Project& p) {
        if (info.hasVideo && info.width > 0 && info.height > 0) {
            p.settings.width = info.width;
            p.settings.height = info.height;
            if (info.fps > 0) fpsFraction(info.fps, &p.settings.fpsNum, &p.settings.fpsDen);
            if (info.fieldOrder == "upper" || info.fieldOrder == "lower") p.settings.fieldOrder = info.fieldOrder;
            p.settings.pixelAspect = 1.0;           // MediaInfo's size is the displayed size
        }
        if (info.hasAudio && info.sampleRate > 0) p.settings.sampleRate = info.sampleRate;
    });
    const uint64_t id = app.importMedia(path, &e);
    if (!id) {
        *error = e;
        return false;
    }
    app.addMediaToTimeline(id, 0.0);
    if (app.project().length() <= 0) {
        *error = "it has no length";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ --vst-params
// Sets each parameter of a VST 2 plug-in from 0 to 1 and records the value text it returns ("-30.0dB").
int runVstParams(const std::string& where)
{
    std::vector<std::string> files;
    QFileInfo fi(QString::fromStdString(where));
    if (fi.isDir()) {
        for (const QFileInfo& f : QDir(fi.absoluteFilePath()).entryInfoList(QStringList() << "*.dll" << "*.so" << "*.vst3", QDir::Files, QDir::Name))
            files.push_back(s(f.absoluteFilePath()));
    } else files.push_back(where);
    std::string text;
    auto line = [&](const std::string& t) { out(t); text += t + "\n"; };
    for (const std::string& f : files) {
        std::string err;
        const std::vector<AudioPluginDesc> descs = describeAudioPluginFile(f, &err);
        if (descs.empty()) { line("== " + f + ": cannot be read (" + err + ")"); continue; }
        for (const AudioPluginDesc& d : descs) {
            std::unique_ptr<AudioPlugin> pl = createAudioPlugin(d, &err);
            if (!pl) { line("== " + f + ": cannot be made (" + err + ")"); continue; }
            line("== " + d.name + "  [" + d.format + " " + vstIdText(d.uid) + "]  " + f);
            const int n = pl->paramCount();
            std::vector<float> start((size_t)std::max(0, n));
            for (int k = 0; k < n; ++k) start[(size_t)k] = pl->paramGet(k);
            for (int k = 0; k < n; ++k) {
                std::string row = strfmt("  %2d %-24s default %.4f:", k, pl->paramName(k).c_str(), start[(size_t)k]);
                for (int i = 0; i <= 40; ++i) {
                    pl->paramSet(k, (float)i / 40.f);
                    row += strfmt(" %.3f=%s", i / 40.0, pl->paramText(k).c_str());
                    row += ";";
                }
                pl->paramSet(k, start[(size_t)k]);
                line(row);
            }
        }
    }
    QFile out_("vst_params.txt");
    if (out_.open(QIODevice::WriteOnly | QIODevice::Truncate)) out_.write(text.data(), (qint64)text.size());
    out("Written to " + s(QFileInfo(out_).absoluteFilePath()));
    return kOk;
}

// ------------------------------------------------------------------------------------------------ --audiofx-report
// Reports every audio effect of a project and its nested projects: plug-in file, whether the saved settings were
// applied, each setting's text, and the gain on a test tone and on noise.
namespace {
std::string floatsOfState(const std::string& st, std::vector<float>& v)
{
    v.clear();
    if (st.size() < 2 || st.compare(0, 2, "P:") != 0) return st.substr(0, 2);
    const std::string raw = base64Decode(st.substr(2));
    v.resize(raw.size() / sizeof(float));
    if (!v.empty()) std::memcpy(v.data(), raw.data(), v.size() * sizeof(float));
    return "P:";
}

// Level (dBFS rms, dBFS peak) of the plug-in's output, measured over the second half of 'seconds'.
void levelThrough(AudioPlugin* pl, int rate, int kind, double& inRms, double& outRms, double& outPeak, int64_t& pos)
{
    const int n = rate * 2, block = 1024;
    std::vector<float> l(block), r(block);
    double sIn = 0, sOut = 0, peak = 0;
    long cnt = 0;
    uint32_t seed = 12345;
    for (int done = 0; done < n; done += block) {
        const int m = std::min(block, n - done);
        for (int i = 0; i < m; ++i) {
            double x;
            if (kind == 0) x = 0.2512 * std::sin(2 * 3.14159265358979 * 1000.0 * (done + i) / rate);     // -12 dBFS peak sine
            else { seed = seed * 1664525u + 1013904223u; x = ((int)(seed >> 9) / 4194304.0 - 1.0) * 0.2177; }  // -18 dBFS rms noise
            l[(size_t)i] = r[(size_t)i] = (float)x;
            if (done + i >= n / 2) sIn += x * x;
        }
        pl->process(l.data(), r.data(), m, pos, true);
        pos += m;
        for (int i = 0; i < m; ++i)
            if (done + i >= n / 2) {
                const double a = l[(size_t)i], b = r[(size_t)i];
                sOut += 0.5 * (a * a + b * b);
                peak = std::max(peak, std::max(std::fabs(a), std::fabs(b)));
                ++cnt;
            }
    }
    auto db = [](double v) { return v > 1e-12 ? 10.0 * std::log10(v) : -120.0; };
    inRms = db(sIn / std::max(1L, cnt));
    outRms = db(sOut / std::max(1L, cnt));
    outPeak = peak > 1e-9 ? 20.0 * std::log10(peak) : -120.0;
}

void reportProject(const Project& p, FxRuntime& fx, const std::string& title, std::string& outText, std::set<std::string>& seen, int depth)
{
    auto line = [&](const std::string& t) { out(t); outText += t + "\n"; };
    line(std::string(depth * 2, ' ') + "== " + title);
    auto slotReport = [&](const FxSlot& sl, const std::string& where) {
        if (sl.kind != "audio") return;
        const std::string ind(depth * 2 + 2, ' ');
        line(ind + where + ": \"" + sl.name + "\"" + (sl.bypass ? " (bypassed)" : "") + "  [" + sl.audio.format + " " + vstIdText(sl.audio.uid) + "]");
        AudioPlugin* pl = fx.audio(sl.id);
        if (!pl) { line(ind + "  NOT MADE: " + fx.error(sl.id)); return; }
        if (efxIsReproduction(pl->desc)) line(ind + "  file: built-in (Reno's reproduction)");
        else line(ind + "  file: " + pl->desc.path + (pl->desc.bridged32 ? " (32-bit, bridged)" : ""));
        const std::string saved = sl.state.isString() ? sl.state.asString() : std::string();
        std::vector<float> want, have;
        const std::string kind = floatsOfState(saved, want);
        const std::string now = pl->saveState();
        floatsOfState(now, have);
        if (kind == "P:") {
            std::string diffs;
            int nd = 0;
            for (size_t k = 0; k < want.size() && k < have.size(); ++k)
                if (std::fabs(want[k] - have[k]) > 1e-4 && nd++ < 8) diffs += strfmt(" #%zu saved %.4f now %.4f;", k, want[k], have[k]);
            line(ind + strfmt("  saved settings: %zu values; the plug-in has %d; ", want.size(), pl->paramCount()) +
                 (nd == 0 ? std::string("all went in") : strfmt("%d DIFFER:", nd) + diffs));
        } else if (!saved.empty()) {
            line(ind + "  saved settings: " + kind + " chunk of " + std::to_string(base64Decode(saved.substr(2)).size()) + " bytes; " +
                 (now == saved ? "read back the same" : "read back differently (" + now.substr(0, 2) + " " + std::to_string(base64Decode(now.size() > 2 ? now.substr(2) : std::string()).size()) + " bytes)"));
        } else line(ind + "  saved settings: none (the plug-in's defaults)");
        for (int k = 0; k < pl->paramCount(); ++k)
            line(ind + strfmt("    %2d %-28s %.4f  %s", k, pl->paramName(k).c_str(), pl->paramGet(k), pl->paramText(k).c_str()));
        if (!sl.bypass) {
            int64_t pos = 0;
            double i0, o0, pk0, i1, o1, pk1;
            levelThrough(pl, p.settings.sampleRate > 0 ? p.settings.sampleRate : 48000, 0, i0, o0, pk0, pos);
            levelThrough(pl, p.settings.sampleRate > 0 ? p.settings.sampleRate : 48000, 1, i1, o1, pk1, pos);
            line(ind + strfmt("  level: 1 kHz tone %.1f -> %.1f dBFS rms (%+.1f dB, peak %.1f dBFS); noise %.1f -> %.1f (%+.1f dB, peak %.1f)",
                              i0, o0, o0 - i0, pk0, i1, o1, o1 - i1, pk1));
            // plug-in that boosts the tone a lot: try applying the settings before processing starts, after, and after a restart
            if (sl.audio.format == "VST2" && o0 - i0 > 6.0 && !saved.empty()) {
                const int rate = p.settings.sampleRate > 0 ? p.settings.sampleRate : 48000;
                for (int variant = 0; variant < 3; ++variant) {
                    std::string e2;
                    std::unique_ptr<AudioPlugin> x = createAudioPlugin(pl->desc, &e2);
                    if (!x) { line(ind + "    (a fresh copy could not be made: " + e2 + ")"); break; }
                    if (variant == 0) { x->loadState(saved); x->activate(rate, FxRuntime::kAudioBlock); }
                    else if (variant == 1) { x->activate(rate, FxRuntime::kAudioBlock); x->loadState(saved); }
                    else { x->loadState(saved); x->activate(rate, FxRuntime::kAudioBlock); x->deactivate(); x->activate(rate, FxRuntime::kAudioBlock); x->loadState(saved); }
                    int64_t q = 0;
                    double a0, b0, c0, a1, b1, c1;
                    levelThrough(x.get(), rate, 0, a0, b0, c0, q);
                    levelThrough(x.get(), rate, 1, a1, b1, c1, q);
                    std::string lastParam;
                    if (x->paramCount() > 0) lastParam = strfmt("; last setting %s %.4f %s", x->paramName(x->paramCount() - 1).c_str(), x->paramGet(x->paramCount() - 1), x->paramText(x->paramCount() - 1).c_str());
                    static const char* const names[3] = {"settings before starting", "settings after starting", "settings again after a restart"};
                    line(ind + strfmt("    try %c (%s): tone %+.1f dB (peak %.1f dBFS), noise %+.1f dB (peak %.1f)", 'A' + variant, names[variant], b0 - a0, c0, b1 - a1, c1) + lastParam);
                    x->deactivate();
                }
            }
        }
    };
    int ti = 0;
    for (const Track& t : p.tracks) {
        ++ti;
        if (t.type != TrackType::Audio) continue;
        for (const FxSlot& sl : t.fx) slotReport(sl, strfmt("track %d (%s, volume %.2f dB)", ti, t.name.c_str(), 20.0 * std::log10(std::max(1e-6, t.level))));
        for (const Event& e : t.events)
            for (const FxSlot& sl : e.fx) slotReport(sl, strfmt("track %d event at %.3f s", ti, e.start));
    }
    for (const Bus& b : p.buses) for (const FxSlot& sl : b.fx) slotReport(sl, "bus " + b.name);
    for (const FxSlot& sl : p.audioOut) slotReport(sl, "master");
    // nested projects, once each
    for (const MediaItem& m : p.media) {
        if (!isProjectFile(m.path) || !seen.insert(m.path).second) continue;
        Project np;
        const VegasImportResult r = importVegasProject(m.path, np);
        if (!r.ok) { line(std::string(depth * 2 + 2, ' ') + "nested " + m.path + ": cannot be read (" + r.error + ")"); continue; }
        FxRuntime nfx;
        nfx.prepare(np);
        reportProject(np, nfx, "nested " + m.path, outText, seen, depth + 1);
    }
}
}  // namespace

int runAudioFxReport(AppState& app, const std::string& input)
{
    std::shared_ptr<const Project> sp = app.snapshot();
    std::string text;
    std::set<std::string> seen;
    reportProject(*sp, app.fx, input, text, seen, 0);
    QFile f("audiofx_report.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.data(), (qint64)text.size());
    out("Written to " + s(QFileInfo(f).absoluteFilePath()));
    return kOk;
}

// ------------------------------------------------------------------------------------------------ --profile
// Renders the range frame by frame like the playing preview, at 'scale' of the project size, with the profiler
// (engine/profile.h) on, and prints the results to stdout.
int runPreviewProfile(AppState& app, const std::string& input, double from, double to, double scale, int threadsAsked, bool proxies, bool hashFrames, const std::string& hashFile)
{
    std::shared_ptr<const Project> sp = app.snapshot();
    const Project& p = *sp;
    MediaPool pool;
    pool.setUseProxies(proxies);
    pool.setShareFrames(true);                  // as in the preview
    VideoEngine video(pool, app.fx);
    FrameOptions fo;
    fo.interactive = true;
    fo.scale = scale >= 0.999 ? 1.0 : scale;
    const int64_t f0 = p.frameAt(from), f1 = std::max(f0 + 1, p.frameAt(to));
    // several frames at once only if every effect allows it (Playback::parallelSafe)
    int threads = std::max(1, threadsAsked);
    std::string why;
    if (threads > 1) {
        bool safe = nestedVideoParallelSafe(p);
        p.forEachFx([&](const FxSlot& sl) {
            if (!safe || sl.bypass) return;
            if (ofx::Instance* inst = app.fx.video(sl.id))
                if (!inst->plugin || inst->plugin->safety < 1) safe = false;
        });
        if (!safe) {
            threads = 1;
            why = " (asked for " + std::to_string(threadsAsked) + ": a plug-in is not thread safe, as in the editor)";
        }
    }
    out(strfmt("Profiling %s: frames %lld to %lld (%.2f - %.2f s) at %.0f%% size, %d picture thread%s", input.c_str(), (long long)f0, (long long)f1 - 1,
               p.frameTime(f0), p.frameTime(f1), fo.scale * 100.0, threads, threads == 1 ? "" : "s"));
    // the first frame opens files and starts effects: timed but not counted
    profile::setEnabled(false);
    const auto w0 = std::chrono::steady_clock::now();
    ImagePtr first = video.render(p, p.frameTime(f0), fo);
    const double firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
    if (!first) {
        err("the first picture could not be rendered");
        return kRenderFailed;
    }
    profile::reset();
    profile::setEnabled(true);
    std::vector<double> ms((size_t)(f1 - f0), 0.0);
    std::vector<uint64_t> hashes(hashFrames ? (size_t)(f1 - f0) : 0, 0);
    std::atomic<int64_t> next{f0};
    const auto t0 = std::chrono::steady_clock::now();
    auto work = [&] {
        for (;;) {
            if (gCancel.load()) return;
            const int64_t f = next.fetch_add(1);
            if (f >= f1) return;
            const auto a = std::chrono::steady_clock::now();
            ImagePtr img = video.render(p, p.frameTime(f), fo);
            ms[(size_t)(f - f0)] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            if (hashFrames) {                                   // FNV-1a over size, depth and bytes
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](const void* d, size_t n) { const uint8_t* b = (const uint8_t*)d; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } };
                if (img) {
                    const int dims[3] = {img->w, img->h, (int)img->depth};
                    mix(dims, sizeof dims);
                    for (int y = 0; y < img->h; ++y) mix(img->row(y), (size_t)img->w * img->pixelBytes());
                }
                hashes[(size_t)(f - f0)] = h;
            }
            if (gTty && ((f - f0) % 10 == 0)) progressLine(strfmt("  frame %lld of %lld", (long long)(f - f0), (long long)(f1 - f0)));
        }
    };
    std::vector<std::thread> helpers;
    for (int k = 1; k < threads; ++k) helpers.emplace_back(work);
    work();
    for (auto& t : helpers) t.join();
    const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    profile::setEnabled(false);
    endProgressLine();
    if (gCancel.load()) {
        err("stopped");
        return kCancelled;
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double m : ms) sum += m;
    const double n = (double)ms.size();
    const double fps = p.settings.fps();
    std::printf("reno_cli --profile\n");
    std::printf("GPU acceleration (OpenGL effects): %s\n", ofx::Host::get().config.gpu ? "on" : "off");
    std::printf("Vulkan (built-in effects): %s\n", reno::gpu::enabled() ? reno::gpu::status().c_str() : "off");
    std::printf("project   %s\n", input.c_str());
    std::printf("size      %dx%d at %.3f fps; pictures rendered at %dx%d (scale %.2f)%s\n", p.settings.width, p.settings.height, fps, first->w, first->h, fo.scale,
                why.c_str());
    std::printf("range     %.3f - %.3f s, %lld frames, %d picture thread%s\n", p.frameTime(f0), p.frameTime(f1), (long long)(f1 - f0), threads, threads == 1 ? "" : "s");
    std::printf("opening   first picture (opens files, starts plug-ins) %.0f ms, not counted\n", firstMs);
    std::printf("frames    %lld in %.2f s: average %.2f ms a frame, median %.2f, 95%% under %.2f, worst %.2f ms\n", (long long)ms.size(), wallMs / 1000.0, sum / n,
                sorted[sorted.size() / 2], sorted[std::min(sorted.size() - 1, (size_t)(n * 0.95))], sorted.back());
    const double rate = n * 1000.0 / std::max(0.001, wallMs);
    std::printf("speed     %.1f frames a second (the project plays at %.2f): %s\n", rate, fps,
                rate >= fps ? "keeps up" : strfmt("too slow, %.0f%% of real time", 100.0 * rate / fps).c_str());
    if (hashFrames) {
        uint64_t all = 1469598103934665603ull;
        std::string lines;
        for (size_t i = 0; i < hashes.size(); ++i) {
            for (int b = 0; b < 8; ++b) { all ^= (uint8_t)(hashes[i] >> (8 * b)); all *= 1099511628211ull; }
            lines += strfmt("%lld %016llx\n", (long long)(f0 + (int64_t)i), (unsigned long long)hashes[i]);
        }
        if (!hashFile.empty()) writeFile(hashFile, lines);
        std::printf("hash      %016llx over %lld pictures\n", (unsigned long long)all, (long long)hashes.size());
    }
    std::printf("\n%s", profile::formatReport(profile::snapshot(), wallMs).c_str());
    std::fflush(stdout);
    return kOk;
}

// ------------------------------------------------------------------------------------------------ --stills
// Writes the project's picture at each given time as a PNG, at final render quality and full size.
// 'times' is a comma-separated list or a file holding one; an entry is T or name=T (seconds).
int runStills(AppState& app, const std::string& times, const std::string& dir)
{
    std::string text = times;
    if (fileExists(times)) {
        text.clear();
        if (!readFile(times, text)) { err("cannot read " + times); return kUsage; }
    }
    struct Want { std::string name; double t = 0; };
    std::vector<Want> wants;
    std::string tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        Want w;
        std::string num = tok;
        const size_t eq = tok.find('=');
        if (eq != std::string::npos) { w.name = tok.substr(0, eq); num = tok.substr(eq + 1); }
        char* endp = nullptr;
        w.t = std::strtod(num.c_str(), &endp);
        if (num.empty() || (endp && *endp)) { err("--stills: cannot read the time '" + tok + "'"); wants.clear(); wants.push_back(Want()); wants.back().t = -1; }
        else {
            if (w.name.empty()) w.name = strfmt("still_%04d", (int)wants.size());
            wants.push_back(w);
        }
        tok.clear();
    };
    for (char c : text) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') flush();
        else tok += c;
    }
    flush();
    if (wants.empty() || wants[0].t < 0) { if (wants.empty()) err("--stills: no times given"); return kUsage; }
    if (dir.empty()) { err("--stills needs --stills-dir"); return kUsage; }
    if (!QDir().mkpath(q(dir))) { err("the folder " + dir + " could not be made"); return kRenderFailed; }
    std::shared_ptr<const Project> sp = app.snapshot();
    const Project& p = *sp;
    // fx_status.tsv: every effect and transition in the project and whether Reno has the plug-in
    // (a missing plug-in leaves the picture unchanged, so comparisons must skip it)
    {
        std::string status = "# start_s\tlength_s\tscope\tkind\tname\tid\tstatus\twhy\n";
        auto note = [&](double start, double len, const char* scope, const FxSlot& sl) {
            std::string why;
            bool ok = true;
            if (sl.kind == "ofx") ok = findVideoPlugin(sl.pluginId, &why) != nullptr;
            else if (sl.kind == "audio") ok = audioPluginThere(sl.audio, &why);
            else return;
            for (char& c : why) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            status += strfmt("%.6f\t%.6f\t%s\t%s\t%s\t%s\t%s\t%s\n", start, len, scope, sl.kind == "ofx" ? "video" : "audio", fxSlotLabel(sl).c_str(),
                             (sl.kind == "ofx" ? sl.pluginId : sl.audio.key()).c_str(), ok ? "ok" : "missing", why.c_str());
        };
        for (const Track& t : p.tracks) {
            for (const FxSlot& sl : t.fx) note(0, 0, "track", sl);
            for (const Event& e : t.events) {
                for (const FxSlot& sl : e.fx) note(e.start, e.length, "event", sl);
                if (!e.transition.kind.empty()) note(e.start, e.length, "transition", e.transition);
            }
        }
        for (const FxSlot& sl : p.audioOut) note(0, 0, "master", sl);
        for (const FxSlot& sl : p.videoOut) note(0, 0, "videoout", sl);
        QDir().mkpath(q(dir));
        writeFile(dir + "/fx_status.tsv", status);
    }
    MediaPool pool;
    VideoEngine video(pool, app.fx);
    FrameOptions fo;
    fo.interactive = false;
    int failed = 0;
    for (size_t k = 0; k < wants.size(); ++k) {
        if (gCancel.load()) { err("stopped"); return kCancelled; }
        ImagePtr img = video.render(p, wants[k].t + 1e-7, fo);
        if (img && img->depth != PixelDepth::Byte) img = img->converted(PixelDepth::Byte);
        bool ok = false;
        if (img && img->w > 0 && img->h > 0) {
            QImage qi(img->w, img->h, QImage::Format_ARGB32_Premultiplied);        // Byte frames are premultiplied BGRA, bottom row first
            for (int y = 0; y < img->h; ++y) std::memcpy(qi.scanLine(img->h - 1 - y), img->row(y), (size_t)img->w * 4);
            ok = qi.convertToFormat(QImage::Format_ARGB32).save(q(dir + "/" + wants[k].name + ".png"), "PNG");
        }
        if (!ok) { ++failed; err(strfmt("picture at %.3f s (%s) could not be made", wants[k].t, wants[k].name.c_str())); }
        if (gTty) progressLine(strfmt("  picture %d of %d", (int)k + 1, (int)wants.size()));
    }
    endProgressLine();
    out(strfmt("Wrote %d pictures to %s%s", (int)wants.size() - failed, dir.c_str(), failed ? strfmt(" (%d failed)", failed).c_str() : ""));
    return failed ? kRenderFailed : kOk;
}

// ------------------------------------------------------------------------------------------------ steps of main
// Each step returns kContinue or the exit code for main() to return.
const int kContinue = -1;

// Everything the render needs: input, template, output files, range and spec.
struct Job {
    std::string input;
    bool isProject = false;
    rt::Format fmt;
    rt::Tmpl templ;
    bool projectSized = false;      // no template given: first template of the format, at project size and rate
    std::string output;
    QFileInfo outInfo;
    QString stem;                   // output name without extension
    std::string writerPath;         // encoder output (name_%06d.png for an image sequence)
    std::string firstFile;
    std::string sequenceStem;
    QString sequenceExt;                // image extension of a sequence
    double from = 0, to = 0;
    rt::RenderChoices choices;
    RenderSpec spec;
};

std::vector<std::string> readCommandLine(int argc, char** argv)
{
    std::vector<std::string> args;
#ifdef _WIN32
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int k = 1; w && k < n; ++k) args.push_back(narrow(w[k]));
    if (w) LocalFree(w);
    SetConsoleOutputCP(CP_UTF8);
    gTty = _isatty(_fileno(stderr)) != 0;
#else
    for (int k = 1; k < argc; ++k) args.push_back(argv[k]);
    gTty = isatty(fileno(stderr)) != 0;
#endif
    return args;
}

// Reno's log is kept for failures, shown with -v and written to a file with --log.
void setUpLogging(const Options& o)
{
    if (!o.logFile.empty()) gLogFile = std::fopen(o.logFile.c_str(), "wb");
    setLogEcho(false);
    setLogSink([](const std::string& line) {
        std::lock_guard<std::mutex> lk(gOutMutex);
        gLogTail.push_back(line);
        while (gLogTail.size() > 30) gLogTail.pop_front();
        if (gLogFile) {
            std::fprintf(gLogFile, "%s\n", line.c_str());
            std::fflush(gLogFile);
        }
        if (gVerbose && !gQuiet) {
            endProgressLine();
            std::fprintf(stderr, "  log: %s\n", line.c_str());
            std::fflush(stderr);
        }
    });
}

// Process-wide setup also done by reno.exe: VEGAS reader for nested projects, COM, Ctrl+C.
void setUpProcess()
{
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
#ifdef _WIN32
    OleInitialize(nullptr);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    // Titles & Text only draws into frames: no screen needed
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // without a locale Qt warns, then uses UTF-8 anyway
    if (qEnvironmentVariableIsEmpty("LC_ALL") && qEnvironmentVariableIsEmpty("LC_CTYPE") && qEnvironmentVariableIsEmpty("LANG")) qputenv("LC_ALL", "C.UTF-8");
#endif
}

int resolveInput(const Options& o, Job& job)
{
    const QFileInfo fi(q(o.input));
    if (!fi.exists()) {
        err(o.input + " does not exist");
        return kOpenFailed;
    }
    if (fi.isDir()) {
        err(o.input + " is a folder, not a project or media file");
        return kOpenFailed;
    }
    job.input = s(fi.absoluteFilePath());
    job.isProject = isProjectFile(job.input);
    return kContinue;
}

// Done before anything slow so a wrong name fails fast.
int chooseTemplate(const Options& o, Job& job)
{
    std::vector<rt::Format> formats = rt::loadFormats();
    int fi = -1, ti = -1;
    if (!o.templ.empty()) {
        QString why;
        if (!rt::findTemplate(formats, q(o.templ), &fi, &ti, &why)) {
            err(s(why) + " (reno_cli --list-templates lists them)");
            return kUsage;
        }
    } else {
        fi = formatForExtension(formats, o.output.empty() ? std::string() : s(QFileInfo(q(o.output)).suffix()));
        if (fi < 0 && !o.output.empty() && !QFileInfo(q(o.output)).suffix().isEmpty()) {
            err("no format makes ." + s(QFileInfo(q(o.output)).suffix()) + " files; give a template with -t (reno_cli --list-templates)");
            return kUsage;
        }
        if (fi < 0)
            for (size_t k = 0; k < formats.size(); ++k) if (formats[k].key == (o.audioOnly ? "wav" : "avc")) fi = (int)k;
        if (fi < 0 || formats[(size_t)fi].templates.empty()) {
            err("no render format found");
            return kUsage;
        }
        // custom templates come first: take the first built-in one
        ti = 0;
        while (ti + 1 < (int)formats[(size_t)fi].templates.size() && formats[(size_t)fi].templates[(size_t)ti].custom) ++ti;
        job.projectSized = true;
    }
    job.fmt = formats[(size_t)fi];
    job.templ = job.fmt.templates[(size_t)ti];
    if (job.projectSized && job.fmt.hasVideo) {
        job.templ.width = job.templ.height = 0;
        job.templ.fpsNum = 0;
        job.templ.fpsDen = 1;
        job.templ.par = 0;
        job.templ.field = "";
    }
    return kContinue;
}

int chooseOutput(const Options& o, Job& job)
{
    const rt::Format& fmt = job.fmt;
    std::string output = o.output;
    if (output.empty()) {
        const QFileInfo in(q(job.input));
        QString name = in.completeBaseName() + "." + fmt.ext;
        if (in.fileName().compare(name, Qt::CaseInsensitive) == 0 || !job.isProject) name = in.completeBaseName() + "_render." + fmt.ext;
        output = s(in.dir().filePath(name));
    } else if (QFileInfo(q(output)).suffix().isEmpty()) {
        output += "." + s(fmt.ext);
    }
    const QFileInfo of(q(output));
    output = s(of.absoluteFilePath());
    if (of.isDir()) {
        err(output + " is a folder; give a file name");
        return kUsage;
    }
    if (QFileInfo(q(job.input)) == of) {
        err("the output would replace the input (" + output + ")");
        return kUsage;
    }
    if (!fmt.sequence && of.suffix().compare(fmt.ext, Qt::CaseInsensitive) != 0)
        out("Note: the template's format makes ." + s(fmt.ext) + " files; ." + s(of.suffix()) + " decides the container.");
    if (!of.dir().exists() && !QDir().mkpath(of.absolutePath())) {
        err("the folder " + s(of.absolutePath()) + " could not be made");
        return kRenderFailed;
    }
    job.output = output;
    job.outInfo = QFileInfo(q(output));
    job.stem = job.outInfo.completeBaseName();
    job.writerPath = output;
    job.firstFile = output;
    if (fmt.sequence) {
        // image sequence: name_000000.png ...
        job.sequenceExt = rt::pictureExt(job.templ);
        QString escapedStem = job.stem;
        escapedStem.replace("%", "%%");
        job.writerPath = s(job.outInfo.dir().filePath(escapedStem + "_%06d." + job.sequenceExt));
        job.firstFile = s(job.outInfo.dir().filePath(job.stem + "_000000." + job.sequenceExt));
        job.sequenceStem = s(job.outInfo.dir().filePath(job.stem + "_"));
    }
    if (QFileInfo::exists(q(job.firstFile)) && !o.overwrite) {
        err(job.firstFile + " already exists (use -y / --overwrite to replace it)");
        return kExists;
    }
    return kContinue;
}

// Loads preferences and wires up what plug-ins and nested projects need.
void setUpHost(const Options& o, AppState& app)
{
    app.loadPreferences();
    if (o.gpu >= 0 && ofx::Host::get().config.gpu != (o.gpu == 1)) {
        ofx::Host::get().config.gpu = o.gpu == 1;
        ofx::Host::get().applyConfig();
    }
    if (!o.gpuApi.empty()) app::prefsapply::applyGpuApi(o.gpuApi, o.gpu != 0 && ofx::Host::get().config.gpu);
    if (o.vegasFx >= 0) ofx::Host::get().config.preferBuiltinVegasFx = o.vegasFx == 1;
    if (o.efx >= 0) setEfxPreferBuiltin(o.efx == 1);
    if (o.dxFx >= 0) setDxPreferBuiltin(o.dxFx == 1);
    if (o.fullSizeFx) ofx::Host::get().config.previewSizeForAll = false;
#ifdef _WIN32
    if (HWND host = makeHiddenHostWindow()) {
        ofx::Host::get().config.mainWindow = (void*)host;
        ofx::Host::get().applyConfig();
    }
#endif
    // AppState's auto-save timer is a child of the application object
    for (QTimer* timer : QCoreApplication::instance()->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly)) timer->stop();
    setAudioProxyOptions(true, false);      // use existing proxies of nested projects, make none
    // nested projects make their effects on the main thread between frames (as playback.cpp does)
    setMainThreadPoster([](std::function<void()> fn) { AppState::post(std::move(fn)); }, [] {
        QCoreApplication* a = QCoreApplication::instance();
        return !a || QThread::currentThread() == a->thread();
    });
}

// Scans for new or changed plug-in files as the editor does at startup, trying each in a separate process (plugincheck.cpp).
void scanForNewPlugins(const Options& o, const std::string& probe)
{
    const bool haveCache = Catalog::get().loadCache();
    if (!o.noScan && fileExists(probe) && ScanTask::get().start(probe, false, haveCache)) {
        bool said = false;
        auto lastLine = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        for (;;) {
            const ScanTask::State st = ScanTask::get().state();
            if (!st.running) break;
            if (gCancel.load()) {
                ScanTask::get().cancel();
                break;
            }
            if (st.total > 0) {
                if (!said) {
                    out(haveCache ? "Scanning for new plug-ins..." : "Scanning for plug-ins (first run)...");
                    said = true;
                }
                const auto now = std::chrono::steady_clock::now();
                if (gTty || now - lastLine > std::chrono::seconds(5)) {
                    lastLine = now;
                    progressLine(strfmt("  checking %s (%d of %d)", st.what.c_str(), st.done + 1, st.total));
                }
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        ScanTask::get().join();
        if (said) out(strfmt("  plug-in scan done (%d OpenFX, %d audio plug-ins known)", (int)Catalog::get().video().size(), (int)Catalog::get().audio().size()));
    } else if (!haveCache && !o.noScan && !gQuiet) {
        out("Note: Reno has no plug-in list yet and reno_cli could not find itself (" + probe + ") to look for plug-ins;");
        out("      effects are looked for in the standard OpenFX folders only.");
    }
}

// --plugin-path: tries plug-ins in those folders for this run only; the saved plug-in list is restored afterwards.
int scanPluginPaths(const Options& o, const std::string& probe)
{
    std::vector<std::string> dirs;
    for (const std::string& d : o.pluginPaths) {
        const QFileInfo di(q(d));
        if (!di.exists()) {
            err("--plugin-path: " + d + " does not exist");
            return kUsage;
        }
        dirs.push_back(s(di.isDir() ? di.absoluteFilePath() : di.absolutePath()));
    }
    if (!fileExists(probe)) {
        err("--plugin-path: reno_cli could not find itself (" + probe + ") to try the plug-in files");
        return kOpenFailed;
    }
    Catalog& cat = Catalog::get();
    const std::string listFile = pathJoin(userDataDir(), "plugins.json");
    std::string kept;
    const bool hadList = readFile(listFile, kept);
    const std::vector<std::string> keptVideo = cat.extraVideoDirs, keptAudio = cat.extraAudioDirs;
    cat.extraVideoDirs.insert(cat.extraVideoDirs.end(), dirs.begin(), dirs.end());
    cat.extraAudioDirs.insert(cat.extraAudioDirs.end(), dirs.begin(), dirs.end());
    out("Looking for plug-ins in the --plugin-path folders...");
    auto lastLine = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    cat.scan(probe, false, [&](const std::string& what, int done, int total) {
        const auto now = std::chrono::steady_clock::now();
        if (!what.empty() && (gTty || now - lastLine > std::chrono::seconds(5))) {
            lastLine = now;
            progressLine(strfmt("  checking %s (%d of %d)", what.c_str(), done + 1, total));
        }
    }, &gCancel);
    cat.extraVideoDirs = keptVideo;
    cat.extraAudioDirs = keptAudio;
    if (hadList) writeFile(listFile, kept);
    else QFile::remove(q(listFile));
    out(strfmt("  %d OpenFX, %d audio plug-ins known", (int)cat.video().size(), (int)cat.audio().size()));
    return kContinue;
}

// Opens the project or media file as File > Open does and creates its plug-ins; the count goes to *pluginCount.
int openJobInput(const Options& o, AppState& app, const Job& job, int* pluginCount)
{
    out(std::string(job.isProject ? "Opening " : "Opening media file ") + job.input);
    app.fx.onMaking = [&](const FxSlot& slot, int done, int total) {
        ++*pluginCount;
        std::string file = slot.kind == "audio" ? slot.audio.path : std::string();
        if (slot.kind == "ofx") {
            VideoFxEntry e;
            if (Catalog::get().findVideo(slot.pluginId, &e) && !e.builtin) file = e.path;
        }
        const std::string line = strfmt("  loading %s%s (%d of %d)", slotName(slot).c_str(), file.empty() ? "" : (" - " + pathFileName(file)).c_str(), done + 1, total);
        if (gVerbose) out(line);
        else if (gTty) progressLine(line);
    };
    std::string openError;
    const bool opened = job.isProject ? app.open(job.input, &openError) : openMediaFile(job.input, &openError);
    app.fx.onMaking = nullptr;
    if (!opened) {
        err("cannot open " + job.input + ": " + openError);
        return kOpenFailed;
    }
    if (gCancel.load()) {
        err("stopped");
        return kCancelled;
    }
    if (o.previewProfile || o.findMedia)
        app.edit("Find media", [&](Project& p) {
            // look for missing media by file name next to the project
            const std::string dir = job.input.substr(0, job.input.find_last_of("/\\") + 1);
            for (MediaItem& m : p.media) {
                if (m.generated || m.path.compare(0, 6, "synth:") == 0 || !m.offline) continue;
                const std::string alt = dir + m.path.substr(m.path.find_last_of("/\\") + 1);
                if (fileExists(alt)) { m.path = alt; m.offline = false; }
            }
        });
    if (o.noFx)
        app.edit("Bypass All Effects", [](Project& p) {
            auto off = [](FxChain& c) { for (FxSlot& s : c) s.bypass = true; };
            for (Bus& b : p.buses) off(b.fx);
            for (Track& t : p.tracks) {
                off(t.fx);
                for (Event& e : t.events) off(e.fx);
            }
            off(p.videoOut);
            off(p.audioOut);
        });
    return kContinue;
}

void printProjectSummary(const AppState& app, int pluginCount)
{
    const Project& project = app.project();
    const ProjectSettings& ps = project.settings;
    out(strfmt("  %dx%d, %.3f fps, %d Hz, %s long; %d tracks, %d media files%s", ps.width, ps.height, ps.fps(), ps.sampleRate,
               clockText(project.length()).c_str(), (int)project.tracks.size(), (int)project.media.size(),
               pluginCount ? strfmt(", %d plug-in instances", pluginCount).c_str() : ""));
    if (!app.openNotes().empty()) {
        out(strfmt("  %d things in it Reno shows differently from VEGAS%s", (int)app.openNotes().size(), gVerbose ? ":" : " (-v lists them)"));
        if (gVerbose)
            for (const std::string& n : app.openNotes()) out("    " + n);
    }
}

// Reports missing media and plug-ins (including nested projects). Stops only with --strict.
int reportMissing(const Options& o, AppState& app, const Job& job)
{
    const Project& project = app.project();
    Missing missing;
    for (const MediaItem& m : project.media)
        if (m.offline) missing.media.push_back(m.path);
    project.forEachFx([&](const FxSlot& sl) {
        if (sl.kind == "ofx" && !app.fx.video(sl.id)) missing.plugins.push_back(slotName(sl) + ": " + app.fx.error(sl.id));
        else if (sl.kind == "audio" && !app.fx.audio(sl.id)) missing.plugins.push_back(slotName(sl) + ": " + app.fx.error(sl.id));
    });
    std::set<std::string> seen;
    if (job.isProject) seen.insert(toLower(job.input));
    checkNested(project, job.input, seen, missing, 0);
    if (!missing.media.empty()) {
        const std::vector<std::string> lines = counted(missing.media);
        err(strfmt("%d media file%s missing:", (int)lines.size(), lines.size() == 1 ? " is" : "s are"));
        for (const std::string& m : lines) std::fprintf(stderr, "    %s\n", m.c_str());
    }
    if (!missing.plugins.empty()) {
        const std::vector<std::string> lines = counted(missing.plugins);
        err(strfmt("%d effect%s cannot be loaded (%d different):", (int)missing.plugins.size(), missing.plugins.size() == 1 ? "" : "s", (int)lines.size()));
        for (const std::string& m : lines) std::fprintf(stderr, "    %s\n", m.c_str());
    }
    if (!missing.media.empty() || !missing.plugins.empty()) {
        if (o.strict) {
            err("not rendering (--strict)");
            return kStrict;
        }
        err("rendering without them (--strict stops instead)");
    }
    return kContinue;
}

// Time range from --region, --loop-region, --start, --end and --length.
int chooseRange(const Options& o, const Project& project, Job& job)
{
    const ProjectSettings& ps = project.settings;
    double from = 0, to = project.length();
    if (!o.region.empty()) {
        std::vector<const Project::Marker*> regions;
        for (const auto& m : project.markers)
            if (m.region && m.kind.empty()) regions.push_back(&m);
        std::sort(regions.begin(), regions.end(), [](const Project::Marker* a, const Project::Marker* b) { return a->time < b->time; });
        const Project::Marker* hit = nullptr;
        int n = 0;
        if (o.region[0] == '#' && parseInt(o.region.substr(1), 1, 1000000, &n)) {
            if (n <= (int)regions.size()) hit = regions[(size_t)n - 1];
        } else {
            for (const auto* m : regions)
                if (toLower(m->name) == toLower(o.region)) { hit = m; break; }
        }
        if (!hit) {
            std::string names;
            for (size_t k = 0; k < regions.size(); ++k) names += strfmt("%s#%d \"%s\"", k ? ", " : "", (int)k + 1, regions[k]->name.c_str());
            err("the project has no region " + o.region + (regions.empty() ? std::string(" (it has no regions)") : " (it has: " + names + ")"));
            return kUsage;
        }
        from = hit->time;
        to = hit->time + hit->length;
    }
    if (!o.loopRegion.empty()) {
        const size_t comma = o.loopRegion.find(',');
        double a = 0, b = 0;
        if (comma == std::string::npos || !parseTime(ps, o.loopRegion.substr(0, comma), &a) || !parseTime(ps, o.loopRegion.substr(comma + 1), &b) || b <= a) {
            err("--loop-region wants START,END with END after START (e.g. 5,12.5 or 00:00:05.000,00:00:12.500)");
            return kUsage;
        }
        from = a;
        to = b;
    }
    if (!o.start.empty()) {
        if (!parseTime(ps, o.start, &from)) { err("--start: cannot read the time '" + o.start + "'"); return kUsage; }
        if (o.end.empty() && o.length.empty() && o.region.empty() && o.loopRegion.empty()) to = project.length();
    }
    if (!o.end.empty() && !parseTime(ps, o.end, &to)) { err("--end: cannot read the time '" + o.end + "'"); return kUsage; }
    if (!o.length.empty()) {
        double len = 0;
        if (!parseTime(ps, o.length, &len) || len <= 0) { err("--length: cannot read the length '" + o.length + "'"); return kUsage; }
        to = from + len;
    }
    if (o.previewProfile && o.start.empty() && o.end.empty() && o.length.empty() && o.region.empty() && o.loopRegion.empty())
        to = std::min(project.length(), from + 10.0);
    if (!(to > from)) {
        if (project.length() <= 0) err("there is nothing to render: the timeline is empty");
        else err(strfmt("there is nothing to render between %.3f and %.3f s", from, to));
        return kOpenFailed;
    }
    job.from = from;
    job.to = to;
    return kContinue;
}

// Applies the command-line overrides (size, rate, bit rate, codecs ...) to the template.
int applyTemplateOptions(const Options& o, const ProjectSettings& ps, Job& job)
{
    rt::Tmpl& t = job.templ;
    const rt::Format& fmt = job.fmt;
    if (o.sizeProject) { t.width = t.height = 0; t.par = 0; }
    if (o.width > 0) { t.width = o.width; t.height = o.height; }
    if (o.onlyWidth || o.onlyHeight) {
        // only one side given: the other keeps the aspect ratio, rounded to even
        const int bw = t.width > 0 ? t.width : ps.width, bh = t.height > 0 ? t.height : ps.height;
        int w = o.onlyWidth, h = o.onlyHeight;
        if (!w) w = std::max(16, (int)std::lround((double)h * bw / std::max(1, bh) / 2.0) * 2);
        if (!h) h = std::max(16, (int)std::lround((double)w * bh / std::max(1, bw) / 2.0) * 2);
        t.width = w;
        t.height = h;
    }
    if (o.fpsProject) { t.fpsNum = 0; t.fpsDen = 1; }
    if (o.fpsNum > 0) { t.fpsNum = o.fpsNum; t.fpsDen = o.fpsDen; }
    if (o.rateProject) t.sampleRate = 0;
    if (o.sampleRate > 0) t.sampleRate = o.sampleRate;
    if (o.channels > 0) t.channels = o.channels;
    if (o.bits > 0) t.bits = o.bits;
    if (o.kbps > 0) {
        t.kbps = o.kbps;
        if (t.rc == "quality") t.rc = "vbr";
        if (o.maxKbps == 0 && t.maxKbps > 0 && t.maxKbps < t.kbps) t.maxKbps = t.kbps * 3 / 2;
    }
    if (o.maxKbps > 0) t.maxKbps = o.maxKbps;
    if (o.cbr) t.rc = "cbr";
    if (o.crf >= 0) { t.rc = "quality"; t.quality = o.crf; }
    if (o.twoPass) t.twoPass = true;
    if (o.audioKbps > 0) t.audioKbps = o.audioKbps;
    if (!o.encodeMode.empty()) t.mode = q(o.encodeMode);
    if (!o.profile.empty()) t.profile = q(o.profile);
    if (!o.level.empty()) t.level = q(o.level);
    if (!o.renderQuality.empty()) t.renderQuality = q(o.renderQuality);
    if (o.audioOnly) {
        if (!fmt.hasAudio) { err(s(fmt.name) + " files have no sound (--audio-only)"); return kUsage; }
        t.video = false;
    }
    if (o.videoOnly) {
        if (!fmt.hasVideo) { err(s(fmt.name) + " files have no pictures (--video-only)"); return kUsage; }
        t.audio = false;
    }
    return kContinue;
}

// Builds the RenderSpec from the template as Render As does, then applies the codec options.
int makeJobSpec(const Options& o, const ProjectSettings& ps, const std::string& input, Job& job)
{
    const rt::Format& fmt = job.fmt;
    rt::RenderChoices& choices = job.choices;
    choices.writerPath = job.writerPath;
    choices.stretch = o.stretch;
    choices.useRotation = o.rotation;
    choices.markers = o.markers && !fmt.sequence;
    if (o.loudness && fmt.hasAudio && job.templ.audio) choices.loudnessLog = s(job.outInfo.dir().filePath(job.stem + "_loudness.txt"));
    choices.summary = o.summary;
    if (o.projectPath && job.isProject) choices.projectPath = input;
    QString specError;
    if (!rt::makeRenderSpec(fmt, job.templ, choices, ps, &job.spec, &specError)) {
        err(s(specError));
        return kUsage;
    }
    RenderSpec& spec = job.spec;
    if (!o.videoCodec.empty()) {
        // the template's profile and level belong to its codec: drop them when the codec changes
        if (o.profile.empty()) spec.enc.profile.clear();
        if (o.level.empty()) spec.enc.level.clear();
        if (kVideoFamilies.count(toLower(o.videoCodec))) {
            spec.enc.videoFamily = toLower(o.videoCodec);
            spec.enc.videoCodec.clear();
        } else {
            spec.enc.videoCodec = o.videoCodec;
        }
    }
    if (!o.audioCodec.empty()) {
        if (kAudioFamilies.count(toLower(o.audioCodec))) {
            spec.enc.audioFamily = toLower(o.audioCodec);
            spec.enc.audioCodec.clear();
        } else {
            spec.enc.audioCodec = o.audioCodec;
        }
    }
    return kContinue;
}

void printRenderPlan(const Project& project, const Job& job)
{
    const ProjectSettings& ps = project.settings;
    const RenderSpec& spec = job.spec;
    int outW = 0, outH = 0;
    renderOutputSize(project, spec, &outW, &outH);
    const int fpsN = spec.fpsNum > 0 ? spec.fpsNum : ps.fpsNum, fpsD = spec.fpsNum > 0 ? spec.fpsDen : ps.fpsDen;
    out("Template: " + s(job.fmt.name) + "/" + s(job.templ.name) + (job.projectSized && job.fmt.hasVideo ? " (at the project's frame size and rate)" : ""));
    if (gVerbose)
        for (const QString& line : rt::templateInfo(job.fmt, job.templ).split('\n')) out("    " + s(line));
    std::string what;
    if (spec.enc.video) what += strfmt("video %dx%d %.3f fps", outW, outH, (double)fpsN / fpsD);
    if (spec.enc.audio) what += strfmt("%saudio %d Hz %s", what.empty() ? "" : ", ", spec.sampleRate > 0 ? spec.sampleRate : ps.sampleRate,
                                       spec.enc.audioChannels == 1 ? "mono" : spec.enc.audioChannels == 6 ? "5.1" : "stereo");
    out("Rendering " + strfmt("%s to %s (%s)", clockText(job.from).c_str(), clockText(job.to).c_str(), what.c_str()));
    out("  to " + (job.fmt.sequence ? job.writerPath : job.output));
}

// Renders on a worker thread as the dialog does (this thread creates nested projects' effects), shows progress
// and reports the result. Returns the exit code.
int renderJob(AppState& app, const Job& job)
{
    const rt::Format& fmt = job.fmt;
    const std::string& output = job.output;
    // delete the old file first so a failed render does not leave it looking current
    if (!fmt.sequence && QFileInfo::exists(q(output))) QFile::remove(q(output));
    struct Shared {
        std::mutex m;
        ExportProgress pr;
        bool have = false;
        std::atomic<bool> done{false};
        bool ok = false;
        std::string error, description;
    };
    auto sh = std::make_shared<Shared>();
    Project copy = *app.snapshot();
    const RenderSpec spec = job.spec;
    const double from = job.from, to = job.to;
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([sh, copy, spec, from, to, &app]() {
        std::string e, desc;
        bool ok = false;
        try {
            ok = renderProjectAs(copy, app.fx, spec, from, to,
                                 [&](const ExportProgress& pr) {
                                     {
                                         std::lock_guard<std::mutex> lk(sh->m);
                                         sh->pr = pr;
                                         sh->have = true;
                                     }
                                     return !gCancel.load();
                                 },
                                 &e, app.mediaOpener, &desc);
        } catch (const std::exception& x) {
            ok = false;
            e = x.what();
        } catch (...) {
            ok = false;
            e = "an unexpected error";
        }
        {
            std::lock_guard<std::mutex> lk(sh->m);
            sh->ok = ok;
            sh->error = e;
            sh->description = desc;
        }
        sh->done.store(true);
    });
    auto lastLine = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    int lastPercent = -1;
    while (!sh->done.load()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ExportProgress pr;
        bool have;
        {
            std::lock_guard<std::mutex> lk(sh->m);
            pr = sh->pr;
            have = sh->have;
        }
        if (!have) continue;
        const auto now = std::chrono::steady_clock::now();
        const double el = std::chrono::duration<double>(now - started).count();
        const int percent = (int)std::floor(pr.fraction * 100.0);
        // terminal: redraw a few times a second; file or pipe: a line every 5% or 10 s
        const bool due = gTty ? now - lastLine > std::chrono::milliseconds(200)
                              : (percent / 5 != lastPercent / 5 || now - lastLine > std::chrono::seconds(10));
        if (!due) continue;
        lastLine = now;
        lastPercent = percent;
        const double fps = el > 0 ? pr.frame / el : 0;
        std::string line = strfmt("  %3d%%  frame %lld of %lld  %.1f fps  elapsed %s", percent, (long long)pr.frame, (long long)pr.frames, fps, clockText(el).c_str());
        if (pr.fraction > 0.01) line += "  left " + clockText(el * (1.0 - pr.fraction) / pr.fraction);
        progressLine(line);
    }
    worker.join();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    auto removeOutput = [&] {
        if (fmt.sequence) {
            for (int k = 0; k < 10000000; ++k)
                if (!QFile::remove(q(job.sequenceStem) + QString("%1").arg(k, 6, 10, QChar('0')) + "." + job.sequenceExt)) break;
        } else {
            QFile::remove(q(output));
        }
        if (!job.choices.loudnessLog.empty()) QFile::remove(q(job.choices.loudnessLog));
    };
    if (gCancel.load() && !sh->ok) {
        removeOutput();
        err("stopped (Ctrl+C); the partial file was deleted");
        return kCancelled;
    }
    if (!sh->ok) {
        removeOutput();
        err("the render failed: " + (sh->error.empty() ? std::string("no reason given") : sh->error));
        if (!gVerbose && !gLogTail.empty()) {
            std::fprintf(stderr, "Reno's last log lines:\n");
            for (const std::string& line : gLogTail) std::fprintf(stderr, "    %s\n", line.c_str());
        }
        return kRenderFailed;
    }
    {
        std::lock_guard<std::mutex> lk(gOutMutex);
        if (!gQuiet && gTty) {
            ExportProgress pr = sh->pr;
            std::string line = strfmt("  100%%  frame %lld of %lld  %.1f fps  elapsed %s", (long long)pr.frames, (long long)pr.frames,
                                      elapsed > 0 ? pr.frames / elapsed : 0.0, clockText(elapsed).c_str());
            if (line.size() < 79) line.append(79 - line.size(), ' ');
            std::fputs(("\r" + line + "\n").c_str(), stderr);
            gProgressShown = false;
        }
    }
    if (!sh->description.empty()) out("  encoder: " + sh->description);
    if (fmt.sequence) {
        int n = 0;
        while (QFileInfo::exists(q(job.sequenceStem) + QString("%1").arg(n, 6, 10, QChar('0')) + "." + job.sequenceExt)) ++n;
        std::printf("Rendered %d pictures %s_000000.%s ... in %s\n", n, job.sequenceStem.c_str(), s(job.sequenceExt).c_str(), clockText(elapsed).c_str());
    } else {
        std::printf("Rendered %s (%s) in %s\n", output.c_str(), sizeText(QFileInfo(q(output)).size()).c_str(), clockText(elapsed).c_str());
    }
    if (!job.choices.loudnessLog.empty()) out("  loudness log: " + job.choices.loudnessLog);
    std::fflush(stdout);
    return kOk;
}

}  // namespace

// ================================================================================================ main
int main(int argc, char** argv)
{
    std::vector<std::string> args = readCommandLine(argc, argv);

    // plug-in checking commands: no Qt, no renderer
    if (!args.empty() && isPluginCheckCommand(args[0])) {
        const std::string cmd = args[0];
        args.erase(args.begin());
        if (cmd == "vegasfxrun" && std::find(args.begin(), args.end(), std::string("builtin")) != args.end()) {
            // built-ins like Titles & Text and Credit Roll draw text with Qt fonts
            int qargc = 1;
            char arg0[] = "reno_cli";
            char* qargv[] = {arg0, nullptr};
            QGuiApplication qapp(qargc, qargv);
            installBuiltinTextRasterizer();
            return runPluginCheckCommand(cmd, args);
        }
        return runPluginCheckCommand(cmd, args);
    }
    // vkcheck: Vulkan devices and GPU effects against their CPU code (no Qt)
    if (!args.empty() && args[0] == "vkcheck") {
        args.erase(args.begin());
        return ofx::builtin::runVkCheck(args);
    }
    if (args.empty()) {
        std::fputs(kHelp, stdout);
        return kUsage;
    }
    Options o;
    std::string problem;
    bool help = false, version = false;
    if (!parseArgs(args, o, &problem, &help, &version)) {
        err(problem);
        return kUsage;
    }
    if (help) {
        std::fputs(kHelp, stdout);
        return kOk;
    }
    if (version) {
        std::printf("reno_cli %s (%s)\n", renoVersion(), mediaBackendVersion().c_str());
        return kOk;
    }
    if (!o.vstParams.empty()) return runVstParams(o.vstParams);
    if (o.previewProfile) {
        // nothing is written; the name only has to pass the usual checks
        o.output = s(QDir::temp().filePath("reno_profile_unused.mp4"));
        o.overwrite = true;
    }
    if (o.threads > 0) setProcessorLimit(o.threads);
    if (!o.hwDecode.empty()) setHardwareDecoding(o.hwDecode == "off" ? HwDecodeMode::Off : HwDecodeMode::Auto, o.hwDecode == "auto" || o.hwDecode == "off" ? std::string() : o.hwDecode);
    // FFmpeg's messages only with -v
    av_log_set_level(gVerbose ? AV_LOG_WARNING : AV_LOG_QUIET);         // before any thread pool or encoder exists
    setUpLogging(o);
    setUpProcess();

    // no window is shown; settings and custom templates are read under Reno's name, same as the editor
    int qargc = 1;
    char arg0[] = "reno_cli";
    char* qargv[] = {arg0, nullptr};
    QGuiApplication qapp(qargc, qargv);
    QGuiApplication::setOrganizationName("Reno");
    QGuiApplication::setApplicationName("Reno");
    installBuiltinTextRasterizer();

    if (o.list) return listTemplates(o.listFilter);

    Job job;
    int code = resolveInput(o, job);
    if (code == kContinue) code = chooseTemplate(o, job);
    if (code == kContinue) code = chooseOutput(o, job);
    if (code != kContinue) return code;

    AppState& app = AppState::get();
    setUpHost(o, app);
    const std::string probe = selfExePath();
    scanForNewPlugins(o, probe);
    if (!o.pluginPaths.empty() && !gCancel.load()) {
        code = scanPluginPaths(o, probe);
        if (code != kContinue) return code;
    }
    if (gCancel.load()) {
        err("stopped");
        return kCancelled;
    }

    int pluginCount = 0;
    code = openJobInput(o, app, job, &pluginCount);
    if (code != kContinue) return code;
    printProjectSummary(app, pluginCount);
    code = reportMissing(o, app, job);
    if (code != kContinue) return code;
    const Project& project = app.project();
    code = chooseRange(o, project, job);
    if (code != kContinue) return code;

    if (o.audioFxReport) return runAudioFxReport(app, job.input);
    if (!o.stills.empty()) return runStills(app, o.stills, o.stillsDir);
    if (o.previewProfile) return runPreviewProfile(app, job.input, job.from, job.to, o.previewScale, o.threads, o.proxies, o.hashFrames, o.hashFile);

    code = applyTemplateOptions(o, project.settings, job);
    if (code == kContinue) code = makeJobSpec(o, project.settings, job.input, job);
    if (code != kContinue) return code;
    printRenderPlan(project, job);

    code = renderJob(app, job);
    if (code != kOk) return code;
    if (gLogFile) std::fclose(gLogFile);
    // skip teardown: plug-ins and nested projects go with the process, and a plug-in failing at shutdown cannot fail a finished render
    std::fflush(nullptr);
#ifdef _WIN32
    ExitProcess(kOk);
#else
    std::_Exit(kOk);
#endif
}
