// The plug-ins known on this machine: found by scanning, remembered between runs. Scanning tries each plug-in file in
// a child process (reno_cli), so one that crashes or hangs is just marked as not working.
#pragma once
#include "../core/base.h"
#include "../core/json.h"
#include "../audiofx/audioplugin.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace reno {

struct VideoFxEntry {
    std::string id, label, group, path;
    int versionMajor = 0, versionMinor = 0;
    bool filter = false, generator = false, transition = false;
    bool ok = false;
    bool builtin = false;       // one of Reno's own (path is ofx::kBuiltinBinaryPath; see ofx/builtins.h)
    std::string error;
};

struct AudioFxEntry {
    AudioPluginDesc desc;
    bool ok = false;
    std::string error;
};

class Catalog {
public:
    static Catalog& get();

    std::vector<VideoFxEntry> video() const;
    std::vector<AudioFxEntry> audio() const;
    bool findVideo(const std::string& id, VideoFxEntry* out) const;

    bool loadCache();                       // from the user data folder; false if there is none
    void saveCache() const;
    bool empty() const;                     // nothing found by a scan (Reno's built-in plug-ins aside)

    // Looks for plug-in files and tries any that are new or have changed since the last scan (all of them if 'full').
    // Calls progress(what is being tried, done, total) as it goes.
    void scan(const std::string& probeExe, bool full,
              const std::function<void(const std::string&, int, int)>& progress, std::atomic<bool>* cancel = nullptr);

    // Whether a scan would find anything new: a plug-in file added, changed or gone since the last scan (or no scan
    // yet). 'what' gets the first such file's name.
    bool changedSinceScan(std::string* what = nullptr) const;
    // Whether trying this plug-in file made the checking process (reno_cli) crash at the last scan.
    bool crashedWhenTried(const std::string& path) const;

    // Without a child process: describes the OpenFX files in 'dirs' right here. For tests and the command line.
    void scanInProcess(const std::vector<std::string>& ofxDirs, const std::vector<std::string>& audioFiles);

    // Extra folders the user added.
    std::vector<std::string> extraVideoDirs, extraAudioDirs;

private:
    mutable std::mutex mutex_;
    std::vector<VideoFxEntry> video_;
    std::vector<AudioFxEntry> audio_;
    Json files_ = Json::object();           // path -> { "stamp": size+time, "result": probe output }
    // How the list was made (catalogScanVersion() then): a list made by an older Reno or for another host name is
    // made again (every file tried again), since what effects say about themselves (their labels, whether they work)
    // depends on what the host gives them.
    std::string scanVersion_;
    void rebuildLocked();
};

// The plug-in scan running on a thread of its own: one at a time, shared by the splash screen at start, Options >
// Look for New Plug-Ins and the status bar when either is sent to the background.
class ScanTask {
public:
    static ScanTask& get();
    ~ScanTask();

    // Starts a scan (Catalog::scan) unless one is running (false then: the one running goes on).
    bool start(const std::string& probeExe, bool full, bool onlyIfChanged);
    // Stops the scan: the plug-in being tried is given up (its checking process killed) and the others not reached
    // keep what was known about them.
    void cancel();
    // Waits for the scan's thread to end (cancel() first to make that quick). Main thread.
    void join();

    struct State {
        bool started = false;       // start() was called
        bool running = false;
        bool finished = false;      // the thread is done (join() then returns at once)
        bool lookedOnly = false;    // finished after finding nothing new (onlyIfChanged): nothing was tried
        bool cancelled = false;
        std::string what;           // the file being tried (or "" while looking)
        int done = 0, total = 0;    // files done, of total (0 until the files are counted)
        std::string failure;        // why the scan stopped, if it failed
        int serial = 0;             // which start() this is about (1, 2, ...)
    };
    State state() const;

private:
    mutable std::mutex mutex_;
    State state_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
};

// What the plug-in list made now would say it was made with: the way Reno tries plug-ins (bumped when that changes
// what is found, e.g. labels read from OpenFX resource files) and the host name and VEGAS extensions effects are told.
std::string catalogScanVersion();

// Where reno_cli is expected: next to the running program.
std::string defaultProbeExe();
// A reno_cli other than the one next to Reno for defaultProbeExe to give (Preferences > Reno > Plug-ins; empty: the usual one).
void setProbeExeOverride(const std::string& path);

}  // namespace reno
