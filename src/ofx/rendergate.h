// A lock for the renders of OpenFX effects that never deadlocks.
//
// Frames render on several threads at once, and some effects must be entered by one thread at a time (render thread safety "unsafe": one render at a time in the whole plug-in;
// "instance safe": one render at a time per instance). An effect holding such a lock asks for its input frames, which may be made by other effects with locks of their own, and two
// threads can each hold one and wait for the other's. With an ordinary mutex that is a deadlock.
//
// A RenderGate records which thread holds which gate and which gate each waiting thread waits for, and refuses (lock() gives false) the one request that would close a circle of
// waiting threads. The caller then leaves that effect out of that frame (shows its input) instead of waiting for ever. It is recursive: the holding thread may take it again.
#pragma once
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace reno {
namespace ofx {

class RenderGate {
public:
    // Takes the gate, waiting for another thread to let go of it. False (nothing taken) if waiting would make this thread
    // and others wait for each other for ever.
    bool lock()
    {
        Registry& r = registry();
        const std::thread::id me = std::this_thread::get_id();
        std::unique_lock<std::mutex> g(r.m);
        for (;;) {
            if (depth_ == 0) {
                owner_ = me;
                depth_ = 1;
                return true;
            }
            if (owner_ == me) {
                ++depth_;
                return true;
            }
            // would waiting for this gate end in a wait for a gate this thread holds? (follow: who holds it, what that
            // thread waits for, who holds that...)
            RenderGate* w = this;
            for (int hops = 0; w && hops < 256; ++hops) {
                const std::thread::id o = w->owner_;
                if (o == me) return false;
                auto it = r.waiting.find(o);
                w = it == r.waiting.end() ? nullptr : it->second;
            }
            r.waiting[me] = this;
            ++r.waits;
            r.cv.wait(g);
            r.waiting.erase(me);
        }
    }
    void unlock()
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> g(r.m);
        if (depth_ > 0 && --depth_ == 0) {
            owner_ = std::thread::id();
            r.cv.notify_all();
        }
    }
    // How many times any thread has had to wait for a gate (for tests and measurements).
    static unsigned long long waits() { Registry& r = registry(); std::lock_guard<std::mutex> g(r.m); return r.waits; }

    class Hold {
    public:
        Hold() {}
        explicit Hold(RenderGate& g) : g_(&g), ok_(g.lock()) {}
        Hold(Hold&& o) noexcept : g_(o.g_), ok_(o.ok_) { o.g_ = nullptr; o.ok_ = false; }
        Hold& operator=(Hold&& o) noexcept
        {
            if (this != &o) {
                release();
                g_ = o.g_;
                ok_ = o.ok_;
                o.g_ = nullptr;
                o.ok_ = false;
            }
            return *this;
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        ~Hold() { release(); }
        bool ok() const { return ok_; }
        void release()
        {
            if (g_ && ok_) g_->unlock();
            g_ = nullptr;
            ok_ = false;
        }
    private:
        RenderGate* g_ = nullptr;
        bool ok_ = false;
    };

private:
    struct Registry {
        std::mutex m;
        std::condition_variable cv;
        std::map<std::thread::id, RenderGate*> waiting;     // thread -> the gate it waits for
        unsigned long long waits = 0;
    };
    static Registry& registry()
    {
        static Registry* r = new Registry();                // never freed: threads may still be leaving at exit
        return *r;
    }
    std::thread::id owner_;
    int depth_ = 0;
};

}  // namespace ofx
}  // namespace reno
