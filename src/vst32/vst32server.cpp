// reno_vst32: the host process for one 32-bit VST2 plug-in (the bridge Reno uses for plug-ins its own 64-bit process
// cannot load; see src/audiofx/vst32proto.h). It is built as a 32-bit Windows program; the same source builds as a
// 64-bit Linux program, which is how the bridge is tested without a 32-bit Windows.
//
// The plug-in is run by the very same host code Reno uses in its own process (vst2host.cpp), so a bridged plug-in
// behaves like a loaded one. Threads:
//   main    the interface thread of the plug-in: runs the requests from Reno (open, parameters, state, programs, the
//           editor window) in order, pumps the window messages the plug-in's editor needs, and every few milliseconds
//           passes on the settings Reno changed, reads the plug-in's settings back and gives its editor an idle call
//   reader  reads Reno's requests off the pipe and hands them to main
//   audio   waits for Reno's event, runs the plug-in over the block in the shared memory, answers with an event
#include "../audiofx/audioplugin.h"
#include "../audiofx/vst32ipc.h"
#include "../core/base.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
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
#else
#endif

using namespace reno;
using namespace reno::vst32;

namespace {

struct Request {
    uint32_t op = 0, seq = 0;
    std::string payload;
};

class Server {
public:
    Endpoint ep;
    std::unique_ptr<AudioPlugin> plugin;
    std::atomic<bool> quitting{false};
    std::atomic<bool> pluginReady{false};       // 'plugin' is open (the audio thread may use it)
    std::thread audioThread, readerThread;
    std::mutex qMutex;
    std::deque<Request> queue;
    std::condition_variable qCv;
#ifdef _WIN32
    HANDLE wake = nullptr;
    HWND host = nullptr;                        // the window the plug-in's editor sits in
    bool floating = false;                      // ... which is a window of its own, not a child of Reno's
    DWORD mainThread = 0;
#endif
    bool editorOpen = false;
    int numParams = 0;

    // ---- settings: Reno's changes in, the plug-in's values out
    void applyDirty()
    {
        if (!plugin || !pluginReady.load()) return;
        std::atomic<uint32_t>* in = shmParamIn(ep.memory());
        std::atomic<uint32_t>* dirty = shmParamDirty(ep.memory());
        const int n = std::min(numParams, (int)kMaxParams);
        for (int i = 0; i < n; ++i)
            if (dirty[i].load(std::memory_order_acquire) && dirty[i].exchange(0)) plugin->paramSet(i, bitsFloat(in[i].load()));
    }
    void refreshOut()
    {
        if (!plugin || !pluginReady.load()) return;
        std::atomic<uint32_t>* out = shmParamOut(ep.memory());
        const int n = std::min(numParams, (int)kMaxParams);
        for (int i = 0; i < n; ++i) out[i].store(floatBits(plugin->paramGet(i)));
        ep.header()->latency.store(plugin->latencySamples());
    }

    // ---- the audio thread
    void audioLoop()
    {
        ShmHeader* h = ep.header();
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        while (true) {
            ep.waitRequest(-1);
            if (h->quit.load()) break;
            const uint32_t frames = std::min<uint32_t>(h->reqFrames, kBlockCapacity);
            const uint32_t chans = h->reqChannels;
            if (plugin && pluginReady.load() && frames > 0) {
                applyDirty();
                float* io[kMaxChannels];
                for (uint32_t c = 0; c < kMaxChannels; ++c) io[c] = shmAudio(ep.memory(), c);
                if (chans >= 6) plugin->processChannels(io, 6, (int)frames, h->reqSamplePos, h->reqPlaying != 0);
                else plugin->process(io[0], io[1], (int)frames, h->reqSamplePos, h->reqPlaying != 0);
                h->latency.store(plugin->latencySamples());
            }
            h->blocks.fetch_add(1);
            ep.postResponse();
        }
    }

    // ---- the pipe: requests to main
    void readerLoop()
    {
        FrameHead head;
        std::string payload;
        while (ep.read(head, payload)) {
            Request r;
            r.op = head.op;
            r.seq = head.seq;
            r.payload.swap(payload);
            {
                std::lock_guard<std::mutex> lk(qMutex);
                queue.push_back(std::move(r));
            }
            qCv.notify_one();
#ifdef _WIN32
            SetEvent(wake);
#endif
        }
        // Reno is gone (or sent rubbish): end
        {
            std::lock_guard<std::mutex> lk(qMutex);
            Request r;
            r.op = OpClose;
            r.seq = 0;
            queue.push_back(std::move(r));
        }
        qCv.notify_one();
#ifdef _WIN32
        SetEvent(wake);
#endif
    }

    // ---- the editor window (Windows: a child window of Reno's, in which the plug-in makes its own; elsewhere nothing)
#ifdef _WIN32
    static LRESULT CALLBACK hostProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
    {
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_CLOSE && !GetParent(w)) { ShowWindow(w, SW_HIDE); return 0; }
        return DefWindowProcW(w, m, wp, lp);
    }
    HWND makeHost(HWND parent)
    {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc;
            std::memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = hostProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"RenoVst32Host";
            wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        // The window must be made with the same kind of dots-per-inch awareness as Reno's window it goes into
        // (Windows does not mix them in one window tree)
        typedef HANDLE (WINAPI* GetCtx)(HWND);
        typedef HANDLE (WINAPI* SetCtx)(HANDLE);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        GetCtx getCtx = u ? (GetCtx)GetProcAddress(u, "GetWindowDpiAwarenessContext") : nullptr;
        SetCtx setCtx = u ? (SetCtx)GetProcAddress(u, "SetThreadDpiAwarenessContext") : nullptr;
        HANDLE old = nullptr;
        if (getCtx && setCtx && parent) {
            HANDLE ctx = getCtx(parent);
            if (ctx) old = setCtx(ctx);
        }
        // no parent notifications: they would be sent to Reno's thread, which may be waiting for this one.
        HWND w = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"RenoVst32Host", L"", (parent ? WS_CHILD : WS_POPUP) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                 0, 0, 400, 300, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (old && setCtx) setCtx(old);
        if (!w && parent) {
            // Reno's window would not take it as a child: the plug-in gets a window of its own beside Reno's
            floating = true;
            w = CreateWindowExW(WS_EX_TOOLWINDOW, L"RenoVst32Host", widen(plugin ? plugin->desc.name : std::string("Plug-in")).c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (w) logf("reno_vst32: could not embed the plug-in window; using its own window");
        }
        return w;
    }
#endif

    void sizeHost(int w, int h)
    {
#ifdef _WIN32
        if (host && w > 0 && h > 0) {
            if (floating) {
                RECT r = {0, 0, w, h};
                AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(host, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(host, GWL_EXSTYLE));
                w = r.right - r.left;
                h = r.bottom - r.top;
            }
            SetWindowPos(host, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
#else
        (void)w; (void)h;
#endif
    }

    void closeEditorNow()
    {
        if (!editorOpen) return;
        if (plugin) plugin->closeEditor();
        editorOpen = false;
#ifdef _WIN32
        if (host) { DestroyWindow(host); host = nullptr; }
        floating = false;
#endif
    }

    // ---- one request
    void reply(const Request& r, const Buf& b) { ep.send(r.op | kReplyFlag, r.seq, b.bytes()); }

    void handle(const Request& r)
    {
        Buf in(r.payload), out;
        switch (r.op) {
        case OpOpen: {
            const std::string path = in.getStr();
            AudioPluginDesc d;
            d.format = "VST2";
            d.path = path;
            std::string err;
            if (plugin) err = "already open";
            else plugin = vst2Create(d, &err);
            if (!plugin) {
                out.u32(0);
                out.str(err.empty() ? "could not be opened" : err);
                reply(r, out);
                break;
            }
            plugin->onEditorResize = [this](int w, int h) {
                sizeHost(w, h);
                Buf n;
                n.i32(w);
                n.i32(h);
                ep.send(NotifyEditorResize, 0, n.bytes());
            };
            plugin->onParamChangedByPlugin = [this](int index) {
                if (index >= 0 && index < numParams && index < (int)kMaxParams && plugin)
                    shmParamOut(ep.memory())[index].store(floatBits(plugin->paramGet(index)));
                Buf n;
                n.i32(index);
                ep.send(NotifyParamChanged, 0, n.bytes());
            };
            numParams = plugin->paramCount();
            ep.header()->numParams.store((uint32_t)numParams);
            pluginReady.store(true);
            refreshOut();
            out.u32(1);
            out.str(std::string());
            out.str(plugin->desc.name);
            out.str(plugin->desc.vendor);
            out.str(plugin->desc.uid);
            out.i32(plugin->desc.numInputs);
            out.i32(plugin->desc.numOutputs);
            out.u32((plugin->desc.hasEditor ? 1u : 0u) | (plugin->desc.isInstrument ? 2u : 0u));
            out.i32(numParams);
            out.i32(plugin->programCount());
            out.i32(plugin->latencySamples());
            reply(r, out);
            break;
        }
        case OpClose:
            closeEditorNow();
            if (r.seq) reply(r, out);
            quitting.store(true);
            break;
        case OpActivate: {
            const double sr = in.getF64();
            const int maxBlock = in.getI32();
            out.u32(plugin && plugin->activate(sr, maxBlock) ? 1 : 0);
            refreshOut();
            reply(r, out);
            break;
        }
        case OpDeactivate:
            if (plugin) plugin->deactivate();
            reply(r, out);
            break;
        case OpSetChannels: {
            const int n = in.getI32();
            const bool ok = plugin && plugin->setChannels(n);
            out.u32(ok ? 1 : 0);
            out.i32(plugin ? plugin->channels() : 2);
            refreshOut();
            reply(r, out);
            break;
        }
        case OpParamName: out.str(plugin ? plugin->paramName(in.getI32()) : std::string()); reply(r, out); break;
        case OpParamText: out.str(plugin ? plugin->paramText(in.getI32()) : std::string()); reply(r, out); break;
        case OpParamGet: out.f32(plugin ? plugin->paramGet(in.getI32()) : 0.f); reply(r, out); break;
        case OpParamSet: {
            const int i = in.getI32();
            const float v = in.getF32();
            if (plugin) plugin->paramSet(i, v);
            reply(r, out);
            break;
        }
        case OpSaveState: out.str(plugin ? plugin->saveState() : std::string()); reply(r, out); break;
        case OpLoadState: {
            const std::string s = in.getStr();
            out.u32(plugin && plugin->loadState(s) ? 1 : 0);
            refreshOut();
            reply(r, out);
            break;
        }
        case OpProgramCount: out.i32(plugin ? plugin->programCount() : 0); reply(r, out); break;
        case OpProgramName: out.str(plugin ? plugin->programName(in.getI32()) : std::string()); reply(r, out); break;
        case OpProgramCurrent: out.i32(plugin ? plugin->currentProgram() : -1); reply(r, out); break;
        case OpProgramSet: {
            const int i = in.getI32();
            out.u32(plugin && plugin->setProgram(i) ? 1 : 0);
            refreshOut();
            reply(r, out);
            break;
        }
        case OpSavePreset: {
            const bool bank = in.getU32() != 0;
            out.str(plugin ? plugin->saveVstPresetFile(bank) : std::string());
            reply(r, out);
            break;
        }
        case OpLoadPreset: {
            const std::string bytes = in.getStr();
            std::string err;
            const bool ok = plugin && plugin->loadVstPresetFile(bytes, &err);
            out.u32(ok ? 1 : 0);
            out.str(err);
            refreshOut();
            reply(r, out);
            break;
        }
        case OpEditorOpen: {
            const uint64_t parent = in.getU64();
            int w = 0, h = 0;
            bool ok = false;
            if (plugin && plugin->hasEditor()) {
                if (editorOpen) closeEditorNow();
#ifdef _WIN32
                floating = false;
                host = makeHost((HWND)(uintptr_t)parent);
                ok = host && plugin->openEditor(host, &w, &h);
                if (!ok && host) { DestroyWindow(host); host = nullptr; }
                if (ok) {
                    sizeHost(w, h);
                    ShowWindow(host, floating ? SW_SHOW : SW_SHOWNA);
                }
#else
                ok = plugin->openEditor((void*)(uintptr_t)(parent ? parent : 1), &w, &h);
#endif
                editorOpen = ok;
            }
            out.u32(ok ? 1 : 0);
            out.i32(w);
            out.i32(h);
            reply(r, out);
            break;
        }
        case OpEditorClose:
            closeEditorNow();
            reply(r, out);
            break;
        case OpLatency: out.i32(plugin ? plugin->latencySamples() : 0); reply(r, out); break;
        case OpPing: reply(r, out); break;
        default: reply(r, out); break;
        }
    }

    // ---- the main thread
    int run()
    {
        ShmHeader* h = ep.header();
        if (h->magic != kShmMagic || h->version != kProtocolVersion) return 3;
        readerThread = std::thread([this] { readerLoop(); });
        audioThread = std::thread([this] { audioLoop(); });
        Buf hello;
        hello.u32(kProtocolVersion);
        ep.send(NotifyHello, 0, hello.bytes());

        auto lastTick = std::chrono::steady_clock::now();
        while (!quitting.load()) {
            std::deque<Request> todo;
#ifdef _WIN32
            MsgWaitForMultipleObjects(1, &wake, FALSE, 10, QS_ALLINPUT);
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            {
                std::lock_guard<std::mutex> lk(qMutex);
                todo.swap(queue);
            }
#else
            {
                std::unique_lock<std::mutex> lk(qMutex);
                if (queue.empty()) qCv.wait_for(lk, std::chrono::milliseconds(10));
                todo.swap(queue);
            }
#endif
            for (const Request& r : todo) {
                applyDirty();
                handle(r);
                if (quitting.load()) break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTick >= std::chrono::milliseconds(10)) {
                lastTick = now;
                applyDirty();
                refreshOut();
                if (editorOpen && plugin) plugin->idle();
            }
        }
        // leave: the audio thread first (it is told by the memory and its event), then the plug-in
        h->quit.store(1);
        ep.postRequest();
        if (audioThread.joinable()) audioThread.join();
        pluginReady.store(false);
        closeEditorNow();
        plugin.reset();
        return 0;
    }
};

}  // namespace

#ifdef _WIN32
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* e)
{
    logf("reno_vst32: crashed (exception 0x%08lX at %p)", (unsigned long)e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    if (args.size() < 2) {
        std::fprintf(stderr, "reno_vst32: the 32-bit VST2 host for Reno. It is started by Reno, not by hand.\n");
        return 2;
    }
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(crashFilter);
    // a log beside the others in the temporary folder: what to send along if a 32-bit plug-in misbehaves
    {
        wchar_t tmp[MAX_PATH + 1];
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH) {
            const std::string logPath = narrow(std::wstring(tmp, n)) + "reno_vst32.log";
            FILE* f = _wfopen(widen(logPath).c_str(), L"ab");
            if (f) {
                std::fseek(f, 0, SEEK_END);
                const bool big = std::ftell(f) > 512 * 1024;
                std::fclose(f);
                if (big) { f = _wfopen(widen(logPath).c_str(), L"wb"); if (f) std::fclose(f); }
            }
            setLogSink([logPath](const std::string& line) {
                FILE* g = _wfopen(widen(logPath).c_str(), L"ab");
                if (g) { std::fprintf(g, "[%lu] %s\n", (unsigned long)GetCurrentProcessId(), line.c_str()); std::fclose(g); }
            });
        }
    }
    setLogEcho(false);
#endif
    // Linux: when Reno goes, the pipe closes and the server ends; PDEATHSIG would end it when the thread that started it ends.
    Server s;
#ifdef _WIN32
    s.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.mainThread = GetCurrentThreadId();
#endif
    std::string err;
    if (!s.ep.attach(args, &err)) {
        logf("reno_vst32: %s", err.c_str());
        return 4;
    }
    const int rc = s.run();
    if (s.readerThread.joinable()) {
        // the reader may still be waiting for the next message: Reno closes the pipe when it goes; leave without it
        s.readerThread.detach();
    }
    std::fflush(nullptr);
    _Exit(rc);
}
