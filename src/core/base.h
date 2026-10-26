// Reno - shared basic types: logging, strings, images, audio buffers. Nothing in src/core, src/ofx, src/audiofx or
// src/engine depends on Qt or FFmpeg, so the engine and the plug-in hosts build the same on Windows and Linux.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace reno {

// ---------------------------------------------------------------- logging
void logf(const char* fmt, ...);
void setLogSink(std::function<void(const std::string&)> sink);     // every log line also goes here
void setLogEcho(bool on);           // whether log lines are written to stderr too (on unless turned off: reno_cli)
std::string strfmt(const char* fmt, ...);
std::string vstrfmt(const char* fmt, va_list ap);

// ---------------------------------------------------------------- strings / paths (UTF-8 everywhere)
std::string toLower(std::string s);
bool endsWithNoCase(const std::string& s, const std::string& suffix);
std::string pathFileName(const std::string& p);       // "a/b/c.txt" -> "c.txt"
std::string pathStem(const std::string& p);           // "a/b/c.txt" -> "c"
std::string pathDir(const std::string& p);            // "a/b/c.txt" -> "a/b"
size_t pathExtensionStart(const std::string& p);       // where the ".txt" of "a/b/c.txt" starts (npos: the name has no extension)
std::string pathLowerExtension(const std::string& p); // "a/b/c.TXT" -> "txt" ("" if none)
std::string pathJoin(const std::string& a, const std::string& b);
bool fileExists(const std::string& p);
bool dirExists(const std::string& p);
std::vector<std::string> listDir(const std::string& dir);          // names only, sorted; empty if it cannot be read
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);
std::string base64Encode(const void* data, size_t n);
std::string base64Decode(const std::string& s);
#ifdef _WIN32
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);
#endif
std::string userDataDir();                            // %APPDATA%\Reno or ~/.config/reno (created)

// ---------------------------------------------------------------- video frames
// Four channels, rows stored bottom row first (the way OpenFX hands frames to effects). 8-bit frames are B,G,R,A in
// memory - the order VEGAS gives its effects, and the order Qt's ARGB32 and Windows bitmaps use; float frames are
// R,G,B,A.
enum class PixelDepth { Byte = 0, Float = 1 };

struct Image {
    int w = 0, h = 0;
    PixelDepth depth = PixelDepth::Byte;
    size_t rowBytes = 0;
    std::unique_ptr<uint8_t, void (*)(void*)> data{nullptr, nullptr};
    // A number no other frame made by make() has (0: none). The Vulkan path (ofx/vkgpu.h) uses it to know that the
    // frame an effect is given is the one the effect before it made and left on the card.
    uint64_t serial = 0;

    static std::shared_ptr<Image> make(int w, int h, PixelDepth d = PixelDepth::Byte, bool clear = true);
    size_t pixelBytes() const { return depth == PixelDepth::Byte ? 4 : 16; }
    size_t byteSize() const { return rowBytes * (size_t)h; }
    uint8_t* row(int y) { return data.get() + rowBytes * (size_t)y; }                // y = 0 is the bottom row
    const uint8_t* row(int y) const { return data.get() + rowBytes * (size_t)y; }
    std::shared_ptr<Image> clone() const;
    std::shared_ptr<Image> converted(PixelDepth d) const;                            // Byte BGRA <-> Float RGBA
    void clear();
};
using ImagePtr = std::shared_ptr<Image>;

// dst = src over dst, both premultiplied, same size and depth (Byte). 'opacity' 0..1 scales src.
void compositeOver(Image& dst, const Image& src, float opacity);
// dst = a*(1-t) + b*t
void blendImages(Image& dst, const Image& a, const Image& b, float t);
// Copies 'src' into the middle of a new w x h frame (transparent border), scaling it to fit inside while keeping its
// shape.
ImagePtr fitImage(const Image& src, int w, int h);

// Runs fn(first, end) over rows [0, rows) split into bands, on the processor's cores at once (a pool of threads kept
// for it), and returns when all are done.
void parallelRows(int rows, size_t perRow, const std::function<void(int first, int end)>& fn);

// Runs fn(0) .. fn(n - 1), each once, at the same time on the same pool of threads (and the calling thread, which
// takes tasks too while it waits), and returns when all are done.
void parallelTasks(int n, const std::function<void(int index)>& fn);
// What parallelTasks has done since the program began (for measurements and tests): the calls that handed work out, the
// tasks they held, and how many of those ran on the pool's helper threads rather than on the thread that asked.
void parallelTaskStats(uint64_t& jobs, uint64_t& tasks, uint64_t& tasksOnHelpers);

// Puts the calling thread a step below normal priority (Windows: below normal; elsewhere: a little nicer).
void lowerThreadPriority();
// The calling thread a little above normal (Windows; the sound's mixer, which must not be kept waiting by the frames' threads)
void raiseThreadPriority();

// How many processors Reno's own helper threads (parallelRows), OpenFX effects asking how many there are, and the
// video encoders and decoders it opens may use: 0 (the default) all of them. reno_cli's --threads sets it, before
// anything has started (parallelRows' pool is made once, at its first use).
void setProcessorLimit(int n);
int processorLimit();
// The processors there are (at least 1), no more than processorLimit() when it is set.
int processorCount();

// ---------------------------------------------------------------- audio
struct AudioBuf {                    // stereo, 32-bit float, not interleaved
    std::vector<float> ch[2];
    int frames() const { return (int)ch[0].size(); }
    void resize(int n) { ch[0].assign((size_t)n, 0.f); ch[1].assign((size_t)n, 0.f); }
};

inline double dbToGain(double db) { return db <= -90.0 ? 0.0 : std::pow(10.0, db / 20.0); }

}  // namespace reno
