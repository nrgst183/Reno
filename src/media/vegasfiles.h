// Files VEGAS keeps beside media, in VEGAS's own formats: ".sfk" peak files (waveforms) and ".veg.sfap0" audio
// proxies (the mix of a nested project). No Qt or FFmpeg dependency.
#pragma once
#include "../core/base.h"
#include <mutex>
#include <memory>
#include <vector>

namespace reno {
namespace vegasfiles {

// ---------------------------------------------------------------- file times
// A file's time of last change as a Windows FILETIME (100-nanosecond steps since 1 January 1601, UTC); 0 if it is not
// there.
int64_t fileTime(const std::string& path);
// A time as an MS-DOS date and time, the way VEGAS stores it in a .sfk: the date in the low 16 bits, the time in the
// high 16 bits, the seconds rounded UP to an even number (as FileTimeToDosDateTime does).
uint32_t dosTime(int64_t fileTime, bool local);

// ---------------------------------------------------------------- .sfk peak files
// Peak file folder (Preferences > Reno > Waveforms and frames); empty means beside the media, as VEGAS does.
void setPeakFolder(const std::string& folder);
std::string peakFolder();
// "song.wav" -> "song.sfk"; anything else gets ".sfk" added ("clip.mp4" -> "clip.mp4.sfk").
std::string peakFilePath(const std::string& mediaPath);
// The peak files there may be for a file, best first.
std::vector<std::string> peakFileCandidates(const std::string& mediaPath);

struct PeakFile {
    int channels = 0;
    int blockFrames = 256;          // samples a peak is nominally made from (VEGAS: always 256)
    int bits = 16;                  // 16, or 8 (signed bytes)
    int64_t frames = 0;             // the file's length in samples (at its own rate)
    uint32_t localStamp = 0;        // dosTime(the media's fileTime, local) when it was made
    uint32_t utcStamp = 0;          // dosTime(the media's fileTime, UTC)
    // For each block, for each channel: the lowest and the highest sample, as 16-bit numbers.
    std::vector<int16_t> minmax;
    int64_t blocks() const { return channels > 0 ? (int64_t)(minmax.size() / (2 * (size_t)channels)) : 0; }
    int16_t lo(int64_t block, int ch) const { return minmax[(size_t)(block * channels + ch) * 2]; }
    int16_t hi(int64_t block, int ch) const { return minmax[(size_t)(block * channels + ch) * 2 + 1]; }
    // The samples block 'b' is made from: [first, end) (VEGAS spreads the blocks evenly over the file and lets each
    // take in the first sample of the next).
    void blockRange(int64_t b, int64_t& first, int64_t& end) const;
};
// How many blocks VEGAS makes for 'frames' samples: one per 256, the last one partial.
int64_t peakBlocksFor(int64_t frames, int blockFrames = 256);

bool readPeakFile(const std::string& path, PeakFile& out, std::string* error = nullptr);
// Written whole or not at all (a temporary file beside it, renamed). False if the folder cannot be written.
bool writePeakFile(const std::string& path, const PeakFile& pf);
// Was 'pf' made from the file as it is now? (VEGAS's test: its stamp is the file's time of last change.)
bool peakFileCurrent(const PeakFile& pf, const std::string& mediaPath);

// Makes a PeakFile from a file's sound given in pieces, in order. The length must be known first (the blocks are
// spread evenly over it); sound past it is ignored, and what is not given stays silent.
bool peakRounding(const std::string& audioCodec);
class PeakBuilder {
public:
    PeakBuilder(int channels, int64_t frames, bool nearest = false);
    // 'n' samples of each channel (ch[0 .. channels)), following those given before.
    void add(const float* const* ch, int n);
    int64_t given() const { return at_; }
    // Blocks finished so far (all of them once the whole length has been given).
    int64_t blocksDone() const { return done_; }
    const PeakFile& peaks() const { return pf_; }
    // Stamps it with the media's time of last change (fileTime()) and gives it.
    PeakFile finish(int64_t mediaFileTime);

private:
    PeakFile pf_;
    int64_t at_ = 0;            // samples given so far
    int64_t done_ = 0;          // blocks finished
    int64_t end_ = 0;           // the end of block done_'s samples (0: not worked out yet)
    bool nearest_ = false;
    std::vector<int> lo_, hi_;  // the block being made (and the one after: blocks share a sample)
};

// ---------------------------------------------------------------- .sfap0 audio proxies (Sony Wave64)
// "<path>.sfap0": stream 0's audio proxy (VEGAS numbers them .sfap0 .. .sfap9; a project has one).
std::string audioProxyPath(const std::string& projectPath);

struct AudioProxyInfo {
    int format = 0;                 // WAVEFORMATEX wFormatTag: 3 (IEEE float) in every file VEGAS made
    int channels = 0, rate = 0, bits = 0;
    int64_t frames = 0;
    int64_t dataOffset = 0;         // where the samples start in the file
    // VEGAS's staleness mark (a chunk of its own after the samples): version 1, the time of last change of the project
    // the proxy was made from, and the sound format it was made in.
    bool stamped = false;
    uint32_t stampVersion = 0;
    int64_t sourceTime = 0;
    int stampChannels = 0, stampRate = 0, stampBits = 0;
};
bool readAudioProxyInfo(const std::string& path, AudioProxyInfo& out, std::string* error = nullptr);
// Does VEGAS take this proxy as the current one for the project last changed at 'projectFileTime'?
bool audioProxyCurrent(const AudioProxyInfo& info, int64_t projectFileTime);

// Writes an audio proxy as VEGAS does: stereo 32-bit float Wave64, the summary list (start time "0" and end time in
// 100 ns, as text) and the staleness mark.
class AudioProxyWriter {
public:
    AudioProxyWriter() = default;
    ~AudioProxyWriter();
    AudioProxyWriter(const AudioProxyWriter&) = delete;
    AudioProxyWriter& operator=(const AudioProxyWriter&) = delete;
    bool open(const std::string& path, int rate, std::string* error = nullptr);
    bool write(const float* left, const float* right, int frames);
    // Completes the file, stamped with the project's time of last change (fileTime() of the .veg, taken before it was
    // read), and renames it over 'path'.
    bool finish(int64_t projectFileTime, std::string* error = nullptr);
    void abort();                   // stops, removing the part written
    int64_t frames() const { return frames_; }

private:
    std::string path_, part_;
    void* file_ = nullptr;          // FILE*
    int rate_ = 0;
    int64_t frames_ = 0;
    std::vector<float> buf_;
};

// Reads an audio proxy's samples from anywhere at once (any thread: reads are positioned, under a lock).
class AudioProxyFile {
public:
    ~AudioProxyFile();
    bool open(const std::string& path, std::string* error = nullptr);
    const AudioProxyInfo& info() const { return info_; }
    const std::string& path() const { return path_; }
    // Samples [start, start + frames) as stereo (mono given on both sides); silence outside the file. False if
    // reading failed.
    bool read(int64_t start, int frames, float* left, float* right);
    void close();
    // A proxy held in memory instead of a file (the engine makes one when the folder cannot be written): the same
    // reads, from the two channels' samples.
    bool openMemory(int rate, std::shared_ptr<const std::vector<float>> left, std::shared_ptr<const std::vector<float>> right);

private:
    bool readAt(int64_t offset, void* to, size_t n);
    std::shared_ptr<const std::vector<float>> memL_, memR_;
    std::string path_;
    AudioProxyInfo info_;
    std::mutex mutex_;
    void* handle_ = nullptr;        // HANDLE on Windows, else an fd stored in it
    std::vector<uint8_t> buf_;
};

}  // namespace vegasfiles
}  // namespace reno
