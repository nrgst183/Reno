// Rendering the project to a file the way File > Render As does.
#pragma once
#include "engine.h"
#include "../media/media.h"

namespace reno {

struct RenderSpec {
    EncodeSettings enc;             // the file and how it is compressed; its size, rates and chapters are set from what follows
    int width = 0, height = 0;      // the file's frame size (0: the project's, turned by the rotation when that is used)
    int fpsNum = 0, fpsDen = 0;     // the file's frame rate (0: the project's)
    int sampleRate = 0;             // the file's sample rate (0: the project's)
    std::string quality;            // the rendering quality: "" the project's (Project Properties), or draft, preview, good, best
    std::string motionBlur;         // the motion blur type: "" the project's, or one of ProjectSettings::motionBlur's
    bool useRotation = false;       // "Use project output rotation setting": the frame turned by ProjectSettings::outputRotation
    bool markers = false;           // "Save project markers in media file": markers and regions in the range become chapters
    bool twoPass = false;           // a first pass that only measures (encoders that can: x264, x265; NVENC does it in one)
    bool transparent = false;       // render with transparency (for formats with an alpha channel)
    std::string loudnessLog;        // a file to write the sound's loudness to ("Generate loudness log"; empty: none)
    // the loudness log's metering mode (EBU R 128, or ATSC A/85); a linear-light project's color space
    // for the file (the Custom Template's Project tab: "" the View transform; colormgmt.h); the file marked as a
    // 360° (equirectangular) video for players (Project Properties' 360° output)
    bool loudnessAtsc = false;
    std::string colorSpace;
    bool spherical = false;
    // Render As's "Enable multichannel mapping": the file's channels, in order, from these sources - the master (bus
    // -1: its two sides, or in a 5.1 project one of its six channels alone: 'channel' 0..5) or a bus's output (its
    // two sides, or with 'mono' its mono downmix).
    struct ChannelSource {
        int64_t bus = -1;
        int channel = -1;
        bool mono = false;
    };
    std::vector<ChannelSource> channelMap;
    // VEGAS's multiple mono rendering of a 5.1 project: each of the master's channels in a mono file of its own
    // ("<name> Left.wav", "<name> Right.wav", "<name> Center.wav", "<name> LFE.wav", "<name> Left Surround.wav",
    // "<name> Right Surround.wav"; a stereo project: Left and Right).
    bool multipleMono = false;
    // Smart rendering: stretches of a source video file that are shown as they are, and whose codec and picture match the
    // file's, are copied instead of compressed again (smartplan.h; docs/NOTES.md says exactly when). Single pass only.
    bool smartRender = false;
};

// The files a multiple-mono render of 'path' makes, for a project of 'channels' channels (2 or 6).
std::vector<std::string> multipleMonoPaths(const std::string& path, int channels);

// What the file will be: the frame size, after the rotation and the project's size are taken into account.
void renderOutputSize(const Project& p, const RenderSpec& spec, int* width, int* height);

// The project's frame turned clockwise by 'degrees' (90, 180 or 270; anything else gives it back unchanged).
ImagePtr rotateImage(const ImagePtr& img, int degrees);

// Renders 'from' .. 'to' (seconds) of the project into spec.enc.path. progress() is called after every frame
// (both passes count) and returns false to stop. False with *error on failure ("stopped" if stopped).
bool renderProjectAs(const Project& p, FxRuntime& fx, const RenderSpec& spec, double from, double to,
                     const std::function<bool(const ExportProgress&)>& progress, std::string* error,
                     MediaPool::Opener opener = MediaPool::Opener(), std::string* description = nullptr);

}  // namespace reno
