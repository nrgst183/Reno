// 32-bit VST2 plug-ins in Reno (64-bit): a plug-in object run by reno_vst32.exe, a 32-bit process of its own.
// Protocol: vst32proto.h. Plumbing: vst32ipc.h. Other side: src/vst32/vst32server.cpp.
//
// A crash or hang of the bridge process never reaches Reno: every wait has a limit, the process is ended if it does
// not answer, and the plug-in then passes sound through (stoppedReason() says why). It is restarted with its last
// state when playback next starts.
#include "audioplugin.h"
#include "vst32ipc.h"
#include "../core/process.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace reno {

using namespace vst32;

// ================================================================================================ what a file is
namespace {
bool readAt(std::ifstream& f, uint64_t at, void* out, size_t n)
{
    f.clear();
    f.seekg((std::streamoff)at);
    f.read((char*)out, (std::streamsize)n);
    return (size_t)f.gcount() == n;
}
template <class T> bool readValue(std::ifstream& f, uint64_t at, T& v) { return readAt(f, at, &v, sizeof(T)); }
}  // namespace

PeKind inspectPeFile(const std::string& path)
{
    PeKind k;
    std::ifstream f(path, std::ios::binary);
    if (!f) return k;
    uint16_t mz = 0;
    uint32_t peAt = 0, sig = 0;
    if (!readValue(f, 0, mz) || mz != 0x5A4D || !readValue(f, 0x3C, peAt) || peAt > 0x1000000 || !readValue(f, peAt, sig) || sig != 0x00004550) return k;
    uint16_t machine = 0, sections = 0, optSize = 0;
    if (!readValue(f, (uint64_t)peAt + 4, machine) || !readValue(f, (uint64_t)peAt + 6, sections) || !readValue(f, (uint64_t)peAt + 20, optSize)) return k;
    k.machine = machine;
    const uint64_t opt = (uint64_t)peAt + 24;
    uint16_t magic = 0;
    if (!readValue(f, opt, magic) || (magic != 0x10B && magic != 0x20B) || optSize < 96) return k;
    const uint64_t dirAt = opt + (magic == 0x10B ? 96 : 112);
    uint32_t exportRva = 0, exportSize = 0;
    if (!readValue(f, dirAt, exportRva) || !readValue(f, dirAt + 4, exportSize)) return k;
    struct Section { uint32_t va, vsize, raw, rawSize; };
    std::vector<Section> secs;
    for (int i = 0; i < std::min<int>(sections, 96); ++i) {
        const uint64_t at = opt + optSize + (uint64_t)i * 40;
        Section s;
        if (!readValue(f, at + 8, s.vsize) || !readValue(f, at + 12, s.va) || !readValue(f, at + 16, s.rawSize) || !readValue(f, at + 20, s.raw)) return k;
        secs.push_back(s);
    }
    auto toFile = [&](uint32_t rva, uint64_t& out) {
        for (const Section& s : secs)
            if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rawSize)) { out = (uint64_t)s.raw + (rva - s.va); return true; }
        return false;
    };
    if (exportRva == 0) { k.exportsKnown = true; return k; }          // exports nothing
    uint64_t expAt = 0;
    uint32_t nNames = 0, namesRva = 0;
    if (!toFile(exportRva, expAt) || !readValue(f, expAt + 24, nNames) || !readValue(f, expAt + 32, namesRva) || nNames > 100000) return k;
    uint64_t namesAt = 0;
    if (!toFile(namesRva, namesAt)) return k;
    for (uint32_t i = 0; i < nNames; ++i) {
        uint32_t nameRva = 0;
        uint64_t nameAt = 0;
        if (!readValue(f, namesAt + (uint64_t)i * 4, nameRva) || !toFile(nameRva, nameAt)) return k;
        char name[32] = {0};
        readAt(f, nameAt, name, 0);
        f.clear();
        f.seekg((std::streamoff)nameAt);
        f.read(name, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        if (!std::strcmp(name, "VSTPluginMain") || !std::strcmp(name, "main") || !std::strcmp(name, "MAIN")) k.exportsVst = true;
    }
    k.exportsKnown = true;
    return k;
}

static bool forced()
{
    const char* e = std::getenv("RENO_VST32_FORCE");
    return e && e[0] == '1';
}

bool vst32Wanted(const std::string& path)
{
    if (forced()) return true;
    if (sizeof(void*) != 8) return false;       // a 32-bit Reno loads them itself
    const std::string ext = pathLowerExtension(path);
    if (ext != "dll") return false;
    return inspectPeFile(path).machine == 0x14C;
}

std::string vst32ServerPath()
{
    if (const char* e = std::getenv("RENO_VST32_SERVER")) if (*e) return e;
    const std::string self = selfExePath();
#ifdef _WIN32
    const std::string p = pathJoin(pathDir(self), "reno_vst32.exe");
#else
    const std::string p = pathJoin(pathDir(self), "reno_vst32");
#endif
    return fileExists(p) ? p : std::string();
}

std::string audioPluginKindNote(const AudioPluginDesc& desc)
{
    if (efxIsReproduction(desc)) return " (built-in, Reno's reproduction)";
    return desc.bridged32 ? " (32-bit, bridged)" : std::string();
}

// ================================================================================================ one running bridge process
namespace {

const int kOpenTimeoutMs = 90000;       // a plug-in may check its licence or load big samples at start
const int kCallTimeoutMs = 15000;

class Session {
public:
    Endpoint ep;
    std::thread reader;
    std::mutex m;                       // the reply slot
    std::condition_variable cv;
    uint32_t waitingSeq = 0;
    bool haveReply = false, hello = false;
    FrameHead replyHead{};
    std::string reply;
    std::atomic<bool> dead{false};
    std::string deadReason;             // (under m)
    std::atomic<uint32_t> nextSeq{1};
    std::mutex callMutex;               // one request at a time
    std::function<void(int, int)> onResize;
    std::function<void(int)> onParam;
    std::string who;                    // for the log
#ifdef _WIN32
    HANDLE replyEvent = nullptr;
#endif

    Session()
    {
#ifdef _WIN32
        replyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
    }
    ~Session()
    {
        stop();
#ifdef _WIN32
        if (replyEvent) CloseHandle(replyEvent);
#endif
    }

    void signal()
    {
#ifdef _WIN32
        SetEvent(replyEvent);
#endif
        cv.notify_all();
    }

    bool start(const std::string& exe, std::string* error)
    {
        if (!ep.start(exe, error)) return false;
        reader = std::thread([this] { readerLoop(); });
        // the process says hello when it is up
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        std::unique_lock<std::mutex> lk(m);
        while (!hello && !dead.load()) {
            if (cv.wait_until(lk, until) == std::cv_status::timeout) break;
        }
        if (!hello) {
            lk.unlock();
            if (error) *error = "the 32-bit plug-in host (" + pathFileName(exe) + ") did not start";
            stop();
            return false;
        }
        return true;
    }

    void readerLoop()
    {
        FrameHead head;
        std::string payload;
        while (ep.read(head, payload)) {
            if (head.op & kReplyFlag) {
                std::lock_guard<std::mutex> lk(m);
                if (head.seq == waitingSeq && !haveReply) {
                    replyHead = head;
                    reply.swap(payload);
                    haveReply = true;
                    signal();
                }
            } else if (head.op == NotifyHello) {
                std::lock_guard<std::mutex> lk(m);
                hello = true;
                signal();
            } else if (head.op == NotifyEditorResize) {
                Buf b(payload);
                const int w = b.getI32(), h = b.getI32();
                if (b.ok() && onResize) onResize(w, h);
            } else if (head.op == NotifyParamChanged) {
                Buf b(payload);
                const int i = b.getI32();
                if (b.ok() && onParam) onParam(i);
            }
        }
        {
            std::lock_guard<std::mutex> lk(m);
            if (!dead.load()) { deadReason = "the 32-bit plug-in host process ended"; dead.store(true); }
            signal();
        }
    }

    // The process is not used again; the first reason is kept.
    void fail(const std::string& why)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            if (!dead.load()) {
                deadReason = why;
                dead.store(true);
                logf("32-bit VST2 %s: %s", who.c_str(), why.c_str());
            }
            signal();
        }
        ep.kill();
    }

    std::string reason()
    {
        std::lock_guard<std::mutex> lk(m);
        return deadReason;
    }

    // Asks and waits for the answer. False if the process is (or becomes) dead, or says something else.
    bool call(uint32_t op, const Buf& req, Buf* out, int timeoutMs = kCallTimeoutMs)
    {
        if (dead.load()) return false;
        std::lock_guard<std::mutex> cl(callMutex);
        if (dead.load()) return false;
        const uint32_t seq = nextSeq.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(m);
            waitingSeq = seq;
            haveReply = false;
#ifdef _WIN32
            ResetEvent(replyEvent);
#endif
        }
        if (!ep.send(op, seq, req.bytes())) { fail("the 32-bit plug-in host stopped answering"); return false; }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m);
                if (haveReply) break;
                if (dead.load()) return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= until) { fail(strfmt("the 32-bit plug-in host did not answer within %d s and was stopped", timeoutMs / 1000)); return false; }
            const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
#ifdef _WIN32
            // While waiting, this thread still answers messages sent to it by other threads (the plug-in's window is
            // a child of one of ours: a message from the other process must not wait for this call to end)
            const DWORD r = MsgWaitForMultipleObjects(1, &replyEvent, FALSE, (DWORD)std::min(ms, 50), QS_SENDMESSAGE);
            if (r == WAIT_OBJECT_0 + 1) { MSG msg; PeekMessageW(&msg, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE); }
#else
            std::unique_lock<std::mutex> lk(m);
            if (!haveReply && !dead.load()) cv.wait_for(lk, std::chrono::milliseconds(std::min(ms, 50)));
#endif
        }
        std::lock_guard<std::mutex> lk(m);
        if (replyHead.op != (op | kReplyFlag)) { return false; }
        if (out) *out = Buf(reply);
        return true;
    }

    void stop()
    {
        if (ep.memory() && !dead.load() && ep.alive()) {
            // asked to leave first (it unloads the plug-in properly), ended if it does not
            ep.send(OpClose, 0, std::string());
            ep.header()->quit.store(1);
            ep.postRequest();
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            while (ep.alive() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ep.kill();
        if (reader.joinable()) reader.join();
        ep.close();
        dead.store(true);
    }
};

// ================================================================================================ the plug-in object
class Bridge : public AudioPlugin {
public:
    std::shared_ptr<Session> session_;      // replaced when the process restarts: always read with atomic_load
    double sampleRate_ = 48000.0;
    int blockSize_ = 1024;
    bool active_ = false;
    int chans_ = 2;
    int np_ = 0, programs_ = 0;
    std::mutex nameMutex_;
    std::vector<std::string> names_;
    std::vector<char> haveName_;
    std::string lastState_;                 // last state loaded or saved: a restarted process is put back to it
    std::atomic<bool> restarting_{false};
    std::thread restartThread_;
    bool wasPlaying_ = false;
    std::chrono::steady_clock::time_point lastRestart_{};
    int restarts_ = 0;
    bool editorWanted_ = false;
    std::string exe_;

    std::shared_ptr<Session> cur() const { return std::atomic_load(&session_); }

    ~Bridge() override
    {
        if (restartThread_.joinable()) restartThread_.join();
        std::shared_ptr<Session> s = cur();
        if (s) {
            if (editorWanted_) { Buf none; s->call(OpEditorClose, none, nullptr, 3000); }
            s->stop();
        }
    }

    void hook(Session& s)
    {
        s.who = desc.name.empty() ? pathFileName(desc.path) : desc.name;
        s.onResize = [this](int w, int h) { if (onEditorResize) onEditorResize(w, h); };
        s.onParam = [this](int i) { if (onParamChangedByPlugin) onParamChangedByPlugin(i); };
    }

    // Starts the process and has it load the plug-in. Fills 'desc' (first time) or checks it is the same plug-in.
    bool openSession(const std::shared_ptr<Session>& s, const std::string& path, bool first, std::string* error)
    {
        if (!s->start(exe_, error)) return false;
        Buf req, resp;
        req.str(path);
        if (!s->call(OpOpen, req, &resp, kOpenTimeoutMs)) {
            if (error) *error = s->reason().empty() ? "the 32-bit plug-in host did not answer" : s->reason();
            s->stop();
            return false;
        }
        if (resp.getU32() == 0) {
            if (error) *error = resp.getStr();
            s->stop();
            return false;
        }
        resp.getStr();
        const std::string name = resp.getStr(), vendor = resp.getStr(), uid = resp.getStr();
        const int in = resp.getI32(), out = resp.getI32();
        const uint32_t flags = resp.getU32();
        const int params = resp.getI32(), programs = resp.getI32();
        resp.getI32();
        if (!resp.ok()) { if (error) *error = "the 32-bit plug-in host's answer made no sense"; s->stop(); return false; }
        if (first) {
            desc.format = "VST2";
            desc.path = path;
            desc.uid = uid;
            desc.name = name.empty() ? pathStem(path) : name;
            desc.vendor = vendor;
            desc.numInputs = in;
            desc.numOutputs = out;
            desc.hasEditor = (flags & 1) != 0;
            desc.isInstrument = (flags & 2) != 0;
            desc.bridged32 = true;
            np_ = params;
            programs_ = programs;
            names_.assign((size_t)std::max(0, params), std::string());
            haveName_.assign((size_t)std::max(0, params), 0);
        } else if (uid != desc.uid || params != np_) {
            if (error) *error = "the plug-in is not the same one any more";
            s->stop();
            return false;
        }
        hook(*s);
        return true;
    }

    bool open(const std::string& path, std::string* error)
    {
        exe_ = vst32ServerPath();
        if (exe_.empty()) {
            if (error) *error = "is a 32-bit plug-in, and reno_vst32.exe (which runs those) was not found beside Reno";
            return false;
        }
        std::shared_ptr<Session> s = std::make_shared<Session>();
        s->who = pathFileName(path);
        if (!openSession(s, path, true, error)) return false;
        std::atomic_store(&session_, s);
        return true;
    }

    std::string stoppedReason() override
    {
        std::shared_ptr<Session> s = cur();
        if (!s || !s->dead.load()) return std::string();
        const std::string r = s->reason();
        return (desc.name.empty() ? std::string("The plug-in") : desc.name) + " stopped: " + (r.empty() ? "its 32-bit host process ended" : r) +
               ". Its sound passes through unchanged until playback is started again.";
    }

    // ---- restart for the next playback (on its own thread: the audio thread must not wait for it)
    void startAgain()
    {
        if (restarting_.exchange(true)) return;
        if (restartThread_.joinable()) restartThread_.join();
        restartThread_ = std::thread([this] {
            std::shared_ptr<Session> old = cur();
            std::string err;
            std::shared_ptr<Session> s = std::make_shared<Session>();
            s->who = old ? old->who : desc.name;
            bool ok = openSession(s, desc.path, false, &err);
            if (ok) {
                Buf none, resp;
                if (chans_ != 2) { Buf c; c.i32(chans_); s->call(OpSetChannels, c, nullptr); }
                if (!lastState_.empty()) { Buf l; l.str(lastState_); s->call(OpLoadState, l, nullptr); }
                // the settings it had (for a plug-in whose state is only its settings)
                if (old && old->ep.memory()) {
                    const int n = std::min(np_, (int)kMaxParams);
                    for (int i = 0; i < n; ++i) {
                        const bool pending = shmParamDirty(old->ep.memory())[i].load() != 0;
                        const float v = bitsFloat((pending ? shmParamIn(old->ep.memory()) : shmParamOut(old->ep.memory()))[i].load());
                        shmParamIn(s->ep.memory())[i].store(floatBits(v));
                        shmParamDirty(s->ep.memory())[i].store(1);
                    }
                }
                if (active_) { Buf a; a.f64(sampleRate_); a.i32(blockSize_); s->call(OpActivate, a, nullptr); }
                std::atomic_store(&session_, s);
                logf("32-bit VST2 %s: started again", desc.name.c_str());
            } else {
                logf("32-bit VST2 %s: could not be started again (%s)", desc.name.c_str(), err.c_str());
            }
            lastRestart_ = std::chrono::steady_clock::now();
            ++restarts_;
            restarting_.store(false);
        });
    }

    // (called by the audio thread every block: whether playback has just started, judged from the blocks before)
    void maybeStartAgain(bool playing, bool wasPlaying)
    {
        const bool edge = playing && !wasPlaying;
        if (!edge || restarting_.load() || restarts_ >= 20) return;
        if (restarts_ > 0 && std::chrono::steady_clock::now() - lastRestart_ < std::chrono::seconds(2)) return;
        startAgain();
    }

    // ---- audio
    bool activate(double sr, int maxBlock) override
    {
        sampleRate_ = sr;
        blockSize_ = std::max(16, maxBlock);
        active_ = true;
        std::shared_ptr<Session> s = cur();
        if (!s || s->dead.load()) return false;
        Buf req;
        req.f64(sr);
        req.i32(maxBlock);
        Buf resp;
        return s->call(OpActivate, req, &resp) && resp.getU32() != 0;
    }
    void deactivate() override
    {
        active_ = false;
        std::shared_ptr<Session> s = cur();
        if (s && !s->dead.load()) { Buf none; s->call(OpDeactivate, none, nullptr); }
    }

    // One hand-over of up to 'frames' (<= the shared memory's size) of 'channels' channels, in place.
    bool handOver(Session& s, float* const* io, int channels, int frames, int64_t pos, bool playing)
    {
        ShmHeader* h = s.ep.header();
        for (int c = 0; c < channels; ++c) std::memcpy(shmAudio(s.ep.memory(), (uint32_t)c), io[c], (size_t)frames * sizeof(float));
        h->reqFrames = (uint32_t)frames;
        h->reqChannels = (uint32_t)channels;
        h->reqPlaying = playing ? 1 : 0;
        h->reqSamplePos = pos;
        s.ep.postRequest();
        // a block may take its own length ten times over, plus a few seconds
        const int limit = 4000 + (int)(10000.0 * frames / std::max(8000.0, sampleRate_));
        const int r = s.ep.waitResponse(limit);
        if (r != 1) {
            s.fail(r < 0 ? "the 32-bit plug-in host process ended" : strfmt("the 32-bit plug-in host did not finish a block of sound within %d ms and was stopped", limit));
            return false;
        }
        for (int c = 0; c < channels; ++c) std::memcpy(io[c], shmAudio(s.ep.memory(), (uint32_t)c), (size_t)frames * sizeof(float));
        return true;
    }

    int chunkFrames() const
    {
        const int bs = std::max(16, blockSize_);
        if (bs >= (int)kBlockCapacity) return (int)kBlockCapacity;
        return (int)kBlockCapacity / bs * bs;       // a whole number of the plug-in's blocks, so pieces split as one call would
    }

    void processChannels(float* const* io, int channels, int frames, int64_t pos, bool playing) override
    {
        if (chans_ != 6 || channels < 6) {
            if (channels >= 2) process(io[0], io[1], frames, pos, playing);
            return;
        }
        std::shared_ptr<Session> s = cur();
        const bool was = wasPlaying_;
        wasPlaying_ = playing;
        if (!s || s->dead.load()) { maybeStartAgain(playing, was); return; }
        if (!active_) return;
        const int chunk = chunkFrames();
        for (int done = 0; done < frames; done += chunk) {
            const int n = std::min(chunk, frames - done);
            float* part[6];
            for (int c = 0; c < 6; ++c) part[c] = io[c] + done;
            if (!handOver(*s, part, 6, n, pos + done, playing)) return;
        }
    }

    void process(float* left, float* right, int frames, int64_t pos, bool playing) override
    {
        std::shared_ptr<Session> s = cur();
        const bool was = wasPlaying_;
        wasPlaying_ = playing;
        if (!s || s->dead.load()) { maybeStartAgain(playing, was); return; }
        if (!active_) return;
        const int chunk = chunkFrames();
        for (int done = 0; done < frames; done += chunk) {
            const int n = std::min(chunk, frames - done);
            float* part[2] = {left + done, right + done};
            if (!handOver(*s, part, 2, n, pos + done, playing)) return;
        }
    }

    int latencySamples() override
    {
        std::shared_ptr<Session> s = cur();
        return s && s->ep.memory() ? s->ep.header()->latency.load() : 0;
    }

    bool setChannels(int n) override
    {
        if (n != 2 && n != 6) return false;
        if (n == chans_) return true;
        std::shared_ptr<Session> s = cur();
        if (!s || s->dead.load()) return n == 2;
        Buf req, resp;
        req.i32(n);
        if (!s->call(OpSetChannels, req, &resp)) return false;
        const bool ok = resp.getU32() != 0;
        chans_ = resp.getI32();
        return ok;
    }
    int channels() const override { return chans_; }

    // ---- parameters
    int paramCount() override { return np_; }
    std::string paramName(int i) override
    {
        if (i < 0 || i >= np_) return std::string();
        {
            std::lock_guard<std::mutex> lk(nameMutex_);
            if (haveName_[(size_t)i]) return names_[(size_t)i];
        }
        std::string name;
        std::shared_ptr<Session> s = cur();
        Buf req, resp;
        req.i32(i);
        if (!s || !s->call(OpParamName, req, &resp)) return std::string();
        name = resp.getStr();
        std::lock_guard<std::mutex> lk(nameMutex_);
        names_[(size_t)i] = name;
        haveName_[(size_t)i] = 1;
        return name;
    }
    std::string paramText(int i) override
    {
        std::shared_ptr<Session> s = cur();
        Buf req, resp;
        req.i32(i);
        if (!s || i < 0 || i >= np_ || !s->call(OpParamText, req, &resp)) return std::string();
        return resp.getStr();
    }
    float paramGet(int i) override
    {
        std::shared_ptr<Session> s = cur();
        if (!s || i < 0 || i >= np_ || !s->ep.memory()) return 0.f;
        if (i < (int)kMaxParams) {
            // what Reno set and the plug-in has not yet been given, else what the plug-in has
            if (shmParamDirty(s->ep.memory())[i].load(std::memory_order_acquire)) return bitsFloat(shmParamIn(s->ep.memory())[i].load());
            return bitsFloat(shmParamOut(s->ep.memory())[i].load());
        }
        Buf req, resp;
        req.i32(i);
        return s->call(OpParamGet, req, &resp) ? resp.getF32() : 0.f;
    }
    void paramSet(int i, float v) override
    {
        std::shared_ptr<Session> s = cur();
        if (!s || i < 0 || i >= np_ || !s->ep.memory()) return;       // (also when stopped: kept for the next process)
        if (s->dead.load() && i >= (int)kMaxParams) return;
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        if (i < (int)kMaxParams) {
            shmParamIn(s->ep.memory())[i].store(floatBits(v));
            shmParamDirty(s->ep.memory())[i].store(1, std::memory_order_release);
            return;
        }
        Buf req;
        req.i32(i);
        req.f32(v);
        s->call(OpParamSet, req, nullptr);
    }

    // ---- state and programs
    std::string saveState() override
    {
        std::shared_ptr<Session> s = cur();
        Buf none, resp;
        if (!s || !s->call(OpSaveState, none, &resp)) return lastState_;       // stopped: the last state
        std::string st = resp.getStr();
        if (!st.empty()) lastState_ = st;
        return st;
    }
    bool loadState(const std::string& state) override
    {
        std::shared_ptr<Session> s = cur();
        Buf req, resp;
        req.str(state);
        if (!s) return false;
        if (!s->call(OpLoadState, req, &resp, 60000)) { lastState_ = state; return false; }
        const bool ok = resp.getU32() != 0;
        if (ok) lastState_ = state;
        return ok;
    }
    int programCount() override { return programs_; }
    std::string programName(int i) override
    {
        std::shared_ptr<Session> s = cur();
        Buf req, resp;
        req.i32(i);
        if (!s || i < 0 || i >= programs_ || !s->call(OpProgramName, req, &resp)) return std::string();
        return resp.getStr();
    }
    int currentProgram() override
    {
        std::shared_ptr<Session> s = cur();
        Buf none, resp;
        return s && s->call(OpProgramCurrent, none, &resp) ? resp.getI32() : -1;
    }
    bool setProgram(int i) override
    {
        std::shared_ptr<Session> s = cur();
        Buf req, resp;
        req.i32(i);
        return s && i >= 0 && i < programs_ && s->call(OpProgramSet, req, &resp) && resp.getU32() != 0;
    }
    std::string saveVstPresetFile(bool bank) override
    {
        std::shared_ptr<Session> s = cur();
        Buf req, resp;
        req.u32(bank ? 1 : 0);
        return s && s->call(OpSavePreset, req, &resp, 60000) ? resp.getStr() : std::string();
    }
    bool loadVstPresetFile(const std::string& bytes, std::string* error) override
    {
        std::shared_ptr<Session> s = cur();
        Buf req, resp;
        req.str(bytes);
        if (!s || !s->call(OpLoadPreset, req, &resp, 60000)) { if (error) *error = "the 32-bit plug-in host is not running"; return false; }
        const bool ok = resp.getU32() != 0;
        const std::string e = resp.getStr();
        if (!ok && error) *error = e;
        return ok;
    }

    // ---- the plug-in's window: made by the plug-in's process, inside Reno's window (a cross-process child window)
    bool hasEditor() override { return desc.hasEditor; }
    bool openEditor(void* parentWindow, int* w, int* h) override
    {
        std::shared_ptr<Session> s = cur();
        if (!desc.hasEditor || !parentWindow || !s) return false;
        Buf req, resp;
        req.u64((uint64_t)(uintptr_t)parentWindow);
        if (!s->call(OpEditorOpen, req, &resp, 60000) || resp.getU32() == 0) return false;
        const int ew = resp.getI32(), eh = resp.getI32();
        if (w) *w = ew;
        if (h) *h = eh;
        editorWanted_ = true;
        return true;
    }
    void closeEditor() override
    {
        std::shared_ptr<Session> s = cur();
        if (!editorWanted_) return;
        editorWanted_ = false;
        if (s && !s->dead.load()) { Buf none; s->call(OpEditorClose, none, nullptr, 10000); }
    }
    void idle() override {}                     // the process gives the window its own idle calls
};

}  // namespace

std::vector<AudioPluginDesc> vst32Describe(const std::string& path, std::string* error)
{
    std::vector<AudioPluginDesc> out;
    // a 32-bit DLL that is not a VST plug-in is rejected without starting anything
    if (!forced()) {
        const PeKind k = inspectPeFile(path);
        if (k.exportsKnown && !k.exportsVst) { if (error) *error = "is not a VST2 plug-in"; return out; }
    }
    std::unique_ptr<Bridge> b(new Bridge());
    if (b->open(path, error)) out.push_back(b->desc);
    return out;
}

std::unique_ptr<AudioPlugin> vst32Create(const AudioPluginDesc& desc, std::string* error)
{
    std::unique_ptr<Bridge> b(new Bridge());
    if (!b->open(desc.path, error)) return nullptr;
    return std::unique_ptr<AudioPlugin>(b.release());
}

}  // namespace reno
