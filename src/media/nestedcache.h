// Preview files of nested projects ("<project>.veg.renopreview"): the frames of a project that the projects using it
// show, rendered once at half its size, for playing a project that uses it without rendering it again. The frames are kept exactly as the renderer made
// them (8-bit premultiplied B,G,R,A, bottom row first), each one compressed on its own without loss (FFmpeg's UT Video,
// or raw where that encoder is missing), so reading one gives back the same bytes and any frame can be read first.
// The engine decides when a file is current (engine.cpp, "nested projects' preview files"); this file only writes and
// reads them.
//
// Layout (numbers little-endian):
//   0   "RNPV"
//   4   u32 version (1)
//   8   u32 width, u32 height          the frames' size
//   16  u32 fps numerator, u32 denominator
//   24  u64 frames
//   32  u64 signature                  what the frames were made from (the engine's previewSignature)
//   40  u32 codec                      0 raw, 1 UT Video (planar G,B,R,A)
//   44  u32 extradata size             the codec's setup bytes, right after this header
//   48  u64 index offset               where the index starts: per frame u64 offset, u32 size, u32 0 (size 0: the
//                                      frame isn't in the file)
//   56  u32 codec tag                  (UT Video tells its kinds apart by it), then 4 bytes 0
//   64  extradata, then the frames, then the index
#pragma once
#include "../core/base.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace reno {

struct NestedCacheInfo {
    int width = 0, height = 0;
    int fpsNum = 0, fpsDen = 1;
    int64_t frames = 0;
    uint64_t signature = 0;
    int codec = 0;
};

// "<project>.renopreview" beside the project, and the place in Reno's own folder used when that folder can't be written.
std::string nestedCachePath(const std::string& projectPath);
std::string nestedCacheFallbackPath(const std::string& projectPath);
// The header of a preview file; false if it isn't one (or is unfinished).
bool readNestedCacheInfo(const std::string& path, NestedCacheInfo& out);

class NestedCacheFile;

// Writes a preview file: frames 0, 1, 2 ... in order, then finish() renames it into place.
class NestedCacheWriter {
public:
    NestedCacheWriter();
    ~NestedCacheWriter();
    NestedCacheWriter(const NestedCacheWriter&) = delete;
    NestedCacheWriter& operator=(const NestedCacheWriter&) = delete;
    // 'info.codec' is ignored: UT Video if FFmpeg has it, else raw.
    bool open(const std::string& path, const NestedCacheInfo& info, std::string* error = nullptr);
    bool write(const Image& frame);         // 8-bit, info's size
    bool skip();                            // the next frame isn't kept
    // The next frame copied from 'from' as it is stored there (canCopy: the same size and kind of compression).
    bool canCopy(const NestedCacheFile& from) const;
    bool copy(NestedCacheFile& from, int64_t frame);
    bool finish(std::string* error = nullptr);
    void abort();                           // stops, removing the part written
    int64_t written() const { return (int64_t)index_.size(); }

private:
    struct Encoder;
    std::string path_, part_;
    void* file_ = nullptr;                  // FILE*
    NestedCacheInfo info_;
    std::string extradata_;
    std::unique_ptr<Encoder> enc_;
    std::vector<std::pair<uint64_t, uint32_t>> index_;
    uint64_t at_ = 0;
    std::vector<uint8_t> buf_;
};

// Reads frames of a preview file, from any thread at once (the bytes are read under a lock, decoded outside it).
class NestedCacheFile {
public:
    NestedCacheFile();
    ~NestedCacheFile();
    NestedCacheFile(const NestedCacheFile&) = delete;
    NestedCacheFile& operator=(const NestedCacheFile&) = delete;
    bool open(const std::string& path, std::string* error = nullptr);
    const NestedCacheInfo& info() const { return info_; }
    const std::string& path() const { return path_; }
    ImagePtr read(int64_t frame);           // null if it can't be read
    bool has(int64_t frame) const;          // whether the file keeps the frame
    void close();

private:
    friend class NestedCacheWriter;
    struct Decoder;
    bool readRaw(int64_t frame, std::vector<uint8_t>& out);
    bool readAt(uint64_t offset, void* to, size_t n);
    std::unique_ptr<Decoder> takeDecoder();
    void giveBack(std::unique_ptr<Decoder> d);
    std::string path_;
    NestedCacheInfo info_;
    std::string extradata_;
    uint32_t tag_ = 0;                      // the codec's tag (UT Video tells its kinds apart by it)
    std::vector<std::pair<uint64_t, uint32_t>> index_;
    std::mutex mutex_;
    void* handle_ = nullptr;                // HANDLE on Windows, else an fd + 1
    std::vector<std::unique_ptr<Decoder>> idle_;
};

}  // namespace reno
