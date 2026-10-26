// The live plug-in instances behind a project's effect slots.
#pragma once
#include "project.h"
#include "../ofx/ofxhost.h"
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <vector>

namespace reno {

class FxRuntime {
public:
    ~FxRuntime();

    // Makes an instance for every slot in the project that has none yet (and gives it the slot's saved state).
    // Main thread. Cheap when there is nothing new.
    void prepare(const Project& p);
    // prepare() a piece at a time, so that making many instances never holds up the thread that makes them.
    bool prepareSome(const Project& p, size_t& next, double budgetMs, bool video = true, bool audio = true);
    // Drops instances whose slot id is not in 'keep'. Main thread.
    void releaseExcept(const std::set<uint64_t>& keep);
    void clear();

    ofx::Instance* video(uint64_t slotId) const;       // null if there is none (not prepared, or it failed)
                                                       // (on a frame thread with a lane, that lane's own instance of the effect if it has one: see prepareLanes)

    // ---- Parallel frames
    // An OpenFX effect is rendered by one thread at a time per instance. A lane instance is created with the live
    // instance's settings and updated whenever they change (video() does that when the thread asks).
    static constexpr int kLanes = 16;
    static void setThreadLane(int lane);                // this thread's lane (0: none)
    static int threadLane();
    // Makes the lane instances (lanes 1 .. lanes-1) of the video effects the project shows from 'from' for 'span'
    // seconds (track, event, transition, generator and project effects), until about 'budgetMs' milliseconds have
    // gone.
    bool prepareLanes(const Project& p, int lanes, double from, double span, double budgetMs);
    // How many lane instances exist, and how many times one was given the live instance's settings again (for tests).
    int laneInstances() const;
    uint64_t laneSyncs() const { return laneSyncs_.load(); }
    AudioPlugin* audio(uint64_t slotId) const;
    // 5.1 projects: the master's effects take the six channels as one stream when the plug-in can.
    AudioPlugin* audioExtra(uint64_t slotId, int pair) const;
    // A lock for the whole use of one video effect instance by a render: its input is set, it draws, its input is let
    // go.
    ofx::RenderGate& renderLock(const ofx::Instance* inst);
    std::string error(uint64_t slotId) const;          // why there is none
    // The reason a plug-in file's effect was not used for this slot and Reno's own built-in of the same identifier
    // made its instance instead (empty when that did not happen).
    std::string fallback(uint64_t slotId) const;
    void setError(uint64_t slotId, const std::string& e);

    // Writes every live instance's current state into the project's slots. Main thread.
    void captureStates(Project& p);
    // Changes whenever any video effect's settings change (for frame caches).
    uint64_t videoRevision() const;

    static constexpr int kAudioBlock = 2048;                // the largest block audio plug-ins are ever handed

    // Held by a render for as long as it uses the instances (VideoEngine::render and eventPicture,
    // AudioEngine::render and renderChannels take one).
    class Hold {
    public:
        explicit Hold(const FxRuntime& f) : f_(f) { f_.holds_.fetch_add(1); }
        ~Hold() { f_.holds_.fetch_sub(1); }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
    private:
        const FxRuntime& f_;
    };

    // Told before prepare() makes each new plug-in instance (slots that have one already are not counted): the slot,
    // how many instances this prepare() has made so far and how many it is going to make.
    std::function<void(const FxSlot& slot, int done, int total)> onMaking;

    // Called (on whatever thread made the change) when a setting of a video effect changes, or one of a setting's
    // properties does (its label, whether it is enabled...).
    std::function<void(uint64_t slotId, ofx::Param* param, const std::string& what)> onVideoParamChanged;

private:
    // The live instance, and the instances the frame threads other than the first render on (prepareLanes).
    struct Lane {
        std::unique_ptr<ofx::Instance> inst;
        std::atomic<uint64_t> synced{0};                // the live instance's revision these settings are from
    };
    struct VideoFx {
        std::unique_ptr<ofx::Instance> inst;
        std::string error;
        ofx::Plugin* plugin = nullptr;                  // what 'inst' was made from (for the lanes' instances)
        std::string context;
        ofx::InstanceSetup setup;
        bool builtinChosen = false;                     // HostConfig::preferBuiltinVegasFx when 'inst' was made
        std::unique_ptr<Lane> lanes[kLanes];
        bool laneFailed[kLanes] = {};
    };
    bool makeLane(uint64_t slotId, int lane);           // (true: done, or nothing to make)
    mutable std::atomic<uint64_t> laneSyncs_{0};
    struct AudioFx {
        std::unique_ptr<AudioPlugin> plugin;
        std::unique_ptr<AudioPlugin> extra[2];          // 5.1 master: the instances for the 2nd and 3rd pair of channels
        AudioPluginDesc desc;                           // what 'plugin' was made from (its file found)
        std::string error;
        int sampleRate = 0;
        std::string keptState;                          // the settings an eFX slot had when its plug-in could not be made (kept for the next one)
        bool efxBuiltin = false;                        // 'plugin' is Reno's version of one of VEGAS's eFX effects (not the installed plug-in)
    };
    // Instances taken out of the maps: destroyed at once if no render holds instances, else kept for later.
    void retire(std::vector<VideoFx>&& v, std::vector<AudioFx>&& a);
    void freeRetired();                                 // destroys the kept ones if no render holds instances now
    void prepareVideo(const FxSlot& s, const Project& p, const char* role, double durationFrames);
    void prepareAudio(const FxSlot& s, const Project& p);
    void prepareAudioExtras(const FxSlot& s, const Project& p);
    void prepareNow(const Project& p);                  // (prepare's work)
    int countNew(const Project& p) const;               // instances prepare() would make (for onMaking)
    void making(const FxSlot& s);                       // tells onMaking
    int preparing_ = 0;                                 // prepare() calls in progress (onMaking's event loop)
    std::shared_ptr<Project> again_;                    // the project of a prepare() called meanwhile
    int madeSoFar_ = 0, toMake_ = 0;                    // for onMaking
    void prepareSurround(const Project& p);             // a 5.1 project's master effects (see prepare())
    mutable std::mutex mutex_;
    std::map<uint64_t, VideoFx> video_;
    std::map<uint64_t, AudioFx> audio_;
    std::map<uint64_t, std::string> errors_;
    std::map<uint64_t, std::string> fallbacks_;         // slots whose plug-in could not make an instance and a built-in did
    std::map<const ofx::Instance*, std::unique_ptr<ofx::RenderGate>> renderLocks_;     // under mutex_
    std::set<uint64_t> surroundTried_;                  // master slots asked to take the 5.1 master's six channels as one
    mutable std::atomic<int> holds_{0};                 // renders holding the instances (Hold)
    std::vector<VideoFx> retiredVideo_;                 // taken away while held (see Hold); under mutex_
    std::vector<AudioFx> retiredAudio_;
};

// Finds and loads the OpenFX effect with this identifier (through the catalog if it knows it, else by looking in the
// standard folders).
ofx::Plugin* findVideoPlugin(const std::string& id, std::string* error);

}  // namespace reno
