// A tiny opt-in profiler for the preview (reno_cli --profile): where the time of a frame goes - in each plug-in, in
// decoding media - so that a slow project can be understood on someone else's machine (with plug-ins we do not have).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <utility>
#include <string>
#include <vector>

namespace reno {
namespace profile {

extern std::atomic<bool> g_on;
inline bool enabled() { return g_on.load(std::memory_order_relaxed); }
void setEnabled(bool on);
void reset();                                   // forgets everything measured

// One plug-in render: 'id' and 'label' name the plug-in, 'scaled' says it worked at the preview's size (else at the
// project's), 'safety' is its render thread safety (0 unsafe: one at a time in the whole process, 1 instance safe, 2 fully safe).
class PluginScope {
public:
    PluginScope(const std::string& id, const std::string& label, bool scaled, int safety);
    ~PluginScope();
    // Time spent waiting for the plug-in to be free (an unsafe one is held to one render at a time).
    static void noteWait(const std::string& id, double ms);
private:
    bool on_;
    const std::string* id_;
    const std::string* label_;
    bool scaled_;
    int safety_;
    std::chrono::steady_clock::time_point t0_;
};

// Reading a frame from a media file (opening it, seeking, decoding, converting).
class MediaScope {
public:
    explicit MediaScope(const std::string& path);
    ~MediaScope();
private:
    bool on_;
    const std::string* path_;
    std::chrono::steady_clock::time_point t0_;
};

// A seek in a media file's video or sound (counted, with the time it took).
void noteSeek(const std::string& path, double ms);
// A video frame decoded from a media file (whether or not anything used it): decoded against used shows work done twice.
void noteDecode(const std::string& path, int64_t pts);

// A frame of the preview (the whole of VideoEngine::render): for the totals and the "compositing / other" row.
class FrameScope {
public:
    FrameScope();
    ~FrameScope();
private:
    bool on_;
    std::chrono::steady_clock::time_point t0_;
};

struct PluginRow {
    std::string id, label;
    double selfMs = 0, inclMs = 0, waitMs = 0;
    uint64_t calls = 0, scaledCalls = 0, fullCalls = 0;
    int safety = 1;
};
struct MediaRow {
    std::string path;
    double selfMs = 0, seekMs = 0;
    uint64_t calls = 0, seeks = 0;
    uint64_t slowReads = 0;                     // reads (in all) that took over 50 ms: the stalls (a seek into a long-GOP file, a reader opened)
    double slowMs = 0, worstMs = 0;             // their time, and the longest read
    uint64_t decodes = 0, again = 0;            // video frames decoded (a read may decode none - the frame was kept - or many), and
                                                // those of them already decoded before (by this or another reader of the file)
    std::map<int, std::pair<uint64_t, uint64_t>> byThread;   // thread (numbered as they first read) -> frames decoded, reads
};
struct Report {
    uint64_t frames = 0;
    double frameMs = 0;                         // all the frames' time, added up over every thread
    std::vector<PluginRow> plugins;             // by self time, largest first
    std::vector<MediaRow> media;
    double pluginSelfMs = 0, mediaSelfMs = 0;
};
Report snapshot();
// The report as plain text. 'wallMs': the time the frames took by the clock (several threads render at once: the rows add up to more).
std::string formatReport(const Report& r, double wallMs);

}  // namespace profile
}  // namespace reno
