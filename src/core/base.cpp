#include "base.h"
#include "cpufeat.h"
#include "pixrows.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <atomic>
#include <filesystem>
#include <fstream>
#ifdef __linux__
#include <cerrno>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <malloc.h>
#endif

namespace fs = std::filesystem;

namespace reno {

#if defined(RENO_HAVE_AVX2_CODE) && defined(_MSC_VER)
// The OS's saved register state (XCR0); clang-cl by instruction (its _xgetbv needs the xsave target and header).
static unsigned long long readXcr0()
{
#  if defined(__clang__)
    unsigned eax = 0, edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((unsigned long long)edx << 32) | eax;
#  else
    return _xgetbv(0);
#  endif
}
#endif

bool cpuHasAvx2()
{
    static const bool has = [] {
        if (std::getenv("RENO_NO_AVX2")) return false;
#if defined(RENO_HAVE_AVX2_CODE)
#  if defined(_MSC_VER)
        // MSVC and clang-cl: cpuid directly (__builtin_cpu_supports needs clang's runtime library, not linked on Windows)
        int r[4];
        __cpuid(r, 0);
        if (r[0] < 7) return false;
        __cpuid(r, 1);
        if (!((r[2] >> 27) & 1) || !((r[2] >> 28) & 1)) return false;       // OSXSAVE, AVX
        if ((readXcr0() & 6) != 6) return false;                            // the OS saves XMM and YMM
        __cpuidex(r, 7, 0);
        return ((r[1] >> 5) & 1) != 0;
#  else
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") != 0;
#  endif
#else
        return false;
#endif
    }();
    return has;
}


// ---------------------------------------------------------------- logging
static std::mutex gLogMutex;
static std::function<void(const std::string&)> gLogSink;
static bool gLogEcho = true;

std::string vstrfmt(const char* fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    if (n <= 0) return std::string();
    std::string s((size_t)n + 1, '\0');
    std::vsnprintf(&s[0], s.size(), fmt, ap);
    s.resize((size_t)n);
    return s;
}

std::string strfmt(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrfmt(fmt, ap);
    va_end(ap);
    return s;
}

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrfmt(fmt, ap);
    va_end(ap);
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lk(gLogMutex);
        if (gLogEcho) {
            std::fprintf(stderr, "%s\n", s.c_str());
            std::fflush(stderr);
        }
        sink = gLogSink;
    }
    if (sink) sink(s);
}

void setLogEcho(bool on)
{
    std::lock_guard<std::mutex> lk(gLogMutex);
    gLogEcho = on;
}

void setLogSink(std::function<void(const std::string&)> sink)
{
    std::lock_guard<std::mutex> lk(gLogMutex);
    gLogSink = std::move(sink);
}

// ---------------------------------------------------------------- strings / paths
std::string toLower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool endsWithNoCase(const std::string& s, const std::string& suffix)
{
    if (s.size() < suffix.size()) return false;
    return toLower(s.substr(s.size() - suffix.size())) == toLower(suffix);
}

static size_t lastSep(const std::string& p)
{
    size_t a = p.find_last_of("/\\");
    return a;
}

std::string pathFileName(const std::string& p)
{
    size_t a = lastSep(p);
    return a == std::string::npos ? p : p.substr(a + 1);
}

std::string pathStem(const std::string& p)
{
    std::string f = pathFileName(p);
    size_t d = f.find_last_of('.');
    return (d == std::string::npos || d == 0) ? f : f.substr(0, d);
}

std::string pathDir(const std::string& p)
{
    size_t a = lastSep(p);
    return a == std::string::npos ? std::string() : p.substr(0, a);
}

size_t pathExtensionStart(const std::string& p)
{
    const size_t dot = p.find_last_of('.');
    const size_t slash = lastSep(p);
    return dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : std::string::npos;
}

std::string pathLowerExtension(const std::string& p)
{
    const size_t dot = pathExtensionStart(p);
    return dot == std::string::npos ? std::string() : toLower(p.substr(dot + 1));
}

std::string pathJoin(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    char last = a[a.size() - 1];
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (last == '/' || last == '\\') return a + b;
    return a + sep + b;
}

static fs::path toFsPath(const std::string& p)
{
#ifdef _WIN32
    return fs::path(widen(p));
#else
    return fs::path(p);
#endif
}

static std::string fromFsPath(const fs::path& p)
{
#ifdef _WIN32
    return narrow(p.wstring());
#else
    return p.string();
#endif
}

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return fs::is_regular_file(toFsPath(p), ec);
}

bool dirExists(const std::string& p)
{
    std::error_code ec;
    return fs::is_directory(toFsPath(p), ec);
}

std::vector<std::string> listDir(const std::string& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    fs::directory_iterator it(toFsPath(dir), fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) out.push_back(fromFsPath(it->path().filename()));
    std::sort(out.begin(), out.end());
    return out;
}

bool readFile(const std::string& path, std::string& out)
{
    std::ifstream f(toFsPath(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)n);
    if (n > 0) f.read(&out[0], n);
    return (bool)f || f.eof();
}

bool writeFile(const std::string& path, const std::string& data)
{
    std::ofstream f(toFsPath(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    return !f.fail();
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16;
        if (i + 1 < n) v |= (unsigned)p[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)p[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? kB64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? kB64[v & 63] : '=');
    }
    return out;
}

std::string base64Decode(const std::string& s)
{
    int T[256];
    for (int i = 0; i < 256; ++i) T[i] = -1;
    for (int i = 0; i < 64; ++i) T[(unsigned char)kB64[i]] = i;
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (T[c] < 0) continue;
        v = (v << 6) | (unsigned)T[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((v >> bits) & 0xFF));
        }
    }
    return out;
}

#ifdef _WIN32
std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

std::string userDataDir()
{
    std::string dir;
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, 32768);
    dir = (n > 0 && n < 32768) ? pathJoin(narrow(buf), "Reno") : std::string("Reno");
#else
    const char* x = std::getenv("XDG_CONFIG_HOME");
    const char* h = std::getenv("HOME");
    if (x && *x) dir = pathJoin(x, "reno");
    else dir = pathJoin(pathJoin(h ? h : ".", ".config"), "reno");
#endif
    std::error_code ec;
    fs::create_directories(toFsPath(dir), ec);
    return dir;
}

// ---------------------------------------------------------------- images
static void* alignedAlloc(size_t n)
{
    if (n == 0) n = 64;
#ifdef _WIN32
    return _aligned_malloc(n, 64);
#else
    void* p = nullptr;
    if (posix_memalign(&p, 64, n) != 0) return nullptr;
    return p;
#endif
}

static void alignedFree(void* p)
{
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

ImagePtr Image::make(int w, int h, PixelDepth d, bool clear)
{
    if (w < 1 || h < 1 || w > 32768 || h > 32768) return nullptr;
    auto img = std::make_shared<Image>();
    static std::atomic<uint64_t> nextSerial{1};
    img->serial = nextSerial.fetch_add(1, std::memory_order_relaxed);
    img->w = w;
    img->h = h;
    img->depth = d;
    img->rowBytes = (size_t)w * img->pixelBytes();
    void* p = alignedAlloc(img->rowBytes * (size_t)h);
    if (!p) return nullptr;
    img->data = std::unique_ptr<uint8_t, void (*)(void*)>((uint8_t*)p, alignedFree);
    if (clear) std::memset(p, 0, img->rowBytes * (size_t)h);
    return img;
}

void Image::clear()
{
    if (data) std::memset(data.get(), 0, byteSize());
}

void (*gImageSyncHook)(const Image&) = nullptr;

void Image::syncFromGpu() const
{
    if (gImageSyncHook) gImageSyncHook(*this);
    gpuHold.reset();
}

ImagePtr Image::clone() const
{
    sync();
    ImagePtr c = make(w, h, depth, false);
    if (c) std::memcpy(c->data.get(), data.get(), byteSize());
    return c;
}

ImagePtr Image::converted(PixelDepth d) const
{
    sync();
    if (d == depth) return clone();
    ImagePtr c = make(w, h, d, false);
    if (!c) return nullptr;
    parallelRows(h, (size_t)w * 4, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        if (d == PixelDepth::Float) {                    // BGRA bytes -> RGBA floats
            const uint8_t* s = row(y);
            float* o = (float*)c->row(y);
            for (int x = 0; x < w; ++x, s += 4, o += 4) {
                o[0] = s[2] * (1.f / 255.f);
                o[1] = s[1] * (1.f / 255.f);
                o[2] = s[0] * (1.f / 255.f);
                o[3] = s[3] * (1.f / 255.f);
            }
        } else {                                         // RGBA floats -> BGRA bytes
            const float* s = (const float*)row(y);
            uint8_t* o = c->row(y);
            for (int x = 0; x < w; ++x, s += 4, o += 4) {
                for (int k = 0; k < 4; ++k) {
                    float v = s[k == 0 ? 2 : (k == 2 ? 0 : k)];
                    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                    o[k] = (uint8_t)(v * 255.f + 0.5f);
                }
            }
        }
    }
    });
    return c;
}

// ---------------------------------------------------------------- parallelRows
namespace {
thread_local bool tInBand = false;                 // this thread is running a band
thread_local bool tIsHelper = false;               // this is one of the pool's threads
std::atomic<uint64_t> gTaskJobs{0}, gTasks{0}, gTasksOnHelpers{0};

class RowPool {
public:
    static RowPool& get()
    {
        static RowPool* p = new RowPool();
        return *p;
    }
    int workers() const { return (int)threads_.size(); }
    struct Job {
        const std::function<void(int, int)>* fn = nullptr;
        int rows = 0, bands = 0;
        bool tasks = false;                         // the bands are whole tasks
        std::atomic<int> next{0};
        std::atomic<int> left{0};
        std::mutex m;
        std::condition_variable done;
    };
    void run(const std::function<void(int, int)>& fn, int rows, int bands, bool tasks = false)
    {
        auto job = std::make_shared<Job>();
        job->fn = &fn;
        job->tasks = tasks;
        job->rows = rows;
        job->bands = bands;
        job->left = bands;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (int k = 1; k < bands; ++k) queue_.push_back(job);
        }
        cv_.notify_all();
        work(*job);                                 // the caller takes bands too
        std::unique_lock<std::mutex> lk(job->m);
        job->done.wait(lk, [&] { return job->left.load() == 0; });
    }

private:
    RowPool()
    {
        const unsigned hw = (unsigned)processorCount();
        const int n = (int)std::min(15u, hw > 1 ? hw - 1 : 0u);
        for (int k = 0; k < n; ++k)
            threads_.emplace_back([this] {
                tIsHelper = true;
                lowerThreadPriority();
                for (;;) {
                    std::shared_ptr<Job> job;
                    {
                        std::unique_lock<std::mutex> lk(m_);
                        cv_.wait(lk, [&] { return !queue_.empty(); });
                        job = queue_.front();
                        queue_.pop_front();
                    }
                    work(*job);
                }
            });
        for (auto& t : threads_) t.detach();
    }
    static void work(Job& job)
    {
        const bool was = tInBand;
        tInBand = !job.tasks;
        for (;;) {
            const int b = job.next.fetch_add(1);
            if (b >= job.bands) break;
            const int first = (int)((int64_t)job.rows * b / job.bands), end = (int)((int64_t)job.rows * (b + 1) / job.bands);
            if (job.tasks && end > first) {
                gTasks.fetch_add((uint64_t)(end - first));
                if (tIsHelper) gTasksOnHelpers.fetch_add((uint64_t)(end - first));
            }
            if (end > first) (*job.fn)(first, end);
            if (job.left.fetch_sub(1) == 1) {
                std::lock_guard<std::mutex> lk(job.m);
                job.done.notify_all();
            }
        }
        tInBand = was;
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::vector<std::thread> threads_;
};
}  // namespace

static std::atomic<int> gProcessorLimit{0};
void setProcessorLimit(int n) { gProcessorLimit.store(std::max(0, n)); }
int processorLimit() { return gProcessorLimit.load(); }
int processorCount()
{
    int n = (int)std::max(1u, std::thread::hardware_concurrency());
    const int limit = gProcessorLimit.load();
    if (limit > 0) n = std::min(n, limit);
    return n;
}

void raiseThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

void lowerThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
    const pid_t tid = (pid_t)syscall(SYS_gettid);
    errno = 0;
    const int now = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno == 0 && now < 2) setpriority(PRIO_PROCESS, (id_t)tid, 2);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
}

void parallelRows(int rows, size_t perRow, const std::function<void(int first, int end)>& fn)
{
    if (rows <= 0) return;
    if (tInBand || rows < 8 || (size_t)rows * perRow < 262144) {
        fn(0, rows);
        return;
    }
    RowPool& pool = RowPool::get();
    if (pool.workers() == 0) {
        fn(0, rows);
        return;
    }
    const int bands = std::min(rows / 4, (pool.workers() + 1) * 2);
    if (bands < 2) {
        fn(0, rows);
        return;
    }
    pool.run(fn, rows, bands);
}

void parallelTaskStats(uint64_t& jobs, uint64_t& tasks, uint64_t& tasksOnHelpers)
{
    jobs = gTaskJobs.load();
    tasks = gTasks.load();
    tasksOnHelpers = gTasksOnHelpers.load();
}

void parallelTasks(int n, const std::function<void(int index)>& fn)
{
    if (n <= 0) return;
    RowPool* pool = (n >= 2 && !tInBand) ? &RowPool::get() : nullptr;
    if (!pool || pool->workers() == 0) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    gTaskJobs.fetch_add(1);
    const std::function<void(int, int)> band = [&](int first, int end) {
        for (int i = first; i < end; ++i) fn(i);
    };
    pool->run(band, n, n, true);
}

void compositeOver(Image& dst, const Image& src, float opacity)
{
    if (dst.w != src.w || dst.h != src.h || dst.depth != src.depth) return;
    if (opacity <= 0.f) return;
    if (opacity > 1.f) opacity = 1.f;
    if (dst.depth == PixelDepth::Byte) {
        const unsigned op = (unsigned)(opacity * 255.f + 0.5f);
        parallelRows(dst.h, (size_t)dst.w * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) pix::overRow(dst.row(y), src.row(y), dst.w, op);
        });
    } else {
        parallelRows(dst.h, (size_t)dst.w * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = (float*)dst.row(y);
            const float* s = (const float*)src.row(y);
            for (int x = 0; x < dst.w; ++x, d += 4, s += 4) {
                float inv = 1.f - s[3] * opacity;
                for (int k = 0; k < 4; ++k) d[k] = s[k] * opacity + d[k] * inv;
            }
        }
        });
    }
}

void blendImages(Image& dst, const Image& a, const Image& b, float t)
{
    if (dst.w != a.w || dst.h != a.h || dst.w != b.w || dst.h != b.h) return;
    if (dst.depth != a.depth || dst.depth != b.depth) return;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    if (dst.depth == PixelDepth::Byte) {
        const unsigned tb = (unsigned)(t * 256.f + 0.5f), ta = 256 - tb;
        if (tb == 0 || tb == 256) {
            const Image& from = tb == 0 ? a : b;
            if (&from != &dst) std::memcpy(dst.data.get(), from.data.get(), dst.byteSize());
            return;
        }
        parallelRows(dst.h, (size_t)dst.w * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) pix::blendRow(dst.row(y), a.row(y), b.row(y), dst.w * 4, ta, tb);
        });
    } else {
        parallelRows(dst.h, (size_t)dst.w * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = (float*)dst.row(y);
            const float* pa = (const float*)a.row(y);
            const float* pb = (const float*)b.row(y);
            for (int i = 0, n = dst.w * 4; i < n; ++i) d[i] = pa[i] * (1.f - t) + pb[i] * t;
        }
        });
    }
}

// Clears what lies around the dw x dh rectangle at (ox, oy) of 'img' (the bars of a fitted frame); the rectangle is written by the caller.
static void clearAround(Image& img, int ox, int oy, int dw, int dh)
{
    const size_t px = img.pixelBytes();
    for (int y = 0; y < img.h; ++y) {
        uint8_t* r = img.row(y);
        if (y < oy || y >= oy + dh) { std::memset(r, 0, img.rowBytes); continue; }
        if (ox > 0) std::memset(r, 0, (size_t)ox * px);
        if (ox + dw < img.w) std::memset(r + (size_t)(ox + dw) * px, 0, (size_t)(img.w - ox - dw) * px);
    }
}

ImagePtr fitImage(const Image& src, int w, int h)
{
    if (src.w < 1 || src.h < 1) return Image::make(w, h, src.depth, true);
    ImagePtr out = Image::make(w, h, src.depth, false);
    if (!out) return out;
    if (src.w == w && src.h == h) {
        std::memcpy(out->data.get(), src.data.get(), src.byteSize());
        return out;
    }
    double s = std::min((double)w / src.w, (double)h / src.h);
    int dw = std::max(1, (int)(src.w * s + 0.5)), dh = std::max(1, (int)(src.h * s + 0.5));
    if (dw > w) dw = w;
    if (dh > h) dh = h;
    const int ox = (w - dw) / 2, oy = (h - dh) / 2;
    clearAround(*out, ox, oy, dw, dh);
    const int ch = 4;
    const bool isByte = src.depth == PixelDepth::Byte;
    if (s == 1.0 && dw == src.w && dh == src.h) {
        // the frame already has the size it is fitted to.
        const size_t px = src.pixelBytes();
        for (int y = 0; y < dh; ++y) std::memcpy(out->row(oy + y) + (size_t)ox * px, src.row(y), (size_t)dw * px);
        return out;
    }
    // When shrinking by more than 2x, average a small box of source pixels so that the result does not shimmer.
    const int box = std::max(1, (int)(1.0 / s + 0.5));
    parallelRows(dh, (size_t)dw * 4 * (size_t)(box * box), [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        double fy = (y + 0.5) / s - 0.5;
        for (int x = 0; x < dw; ++x) {
            double fx = (x + 0.5) / s - 0.5;
            float acc[4] = {0, 0, 0, 0};
            if (box >= 2) {
                int x0 = (int)(x / s), y0 = (int)(y / s), cnt = 0;
                for (int by = 0; by < box; ++by) {
                    int sy = std::min(src.h - 1, y0 + by);
                    for (int bx = 0; bx < box; ++bx) {
                        int sx = std::min(src.w - 1, x0 + bx);
                        if (isByte) {
                            const uint8_t* p = src.row(sy) + sx * 4;
                            for (int k = 0; k < ch; ++k) acc[k] += p[k];
                        } else {
                            const float* p = (const float*)src.row(sy) + sx * 4;
                            for (int k = 0; k < ch; ++k) acc[k] += p[k];
                        }
                        ++cnt;
                    }
                }
                for (int k = 0; k < ch; ++k) acc[k] /= (float)cnt;
            } else {
                int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
                float tx = (float)(fx - x0), ty = (float)(fy - y0);
                int x1 = std::min(src.w - 1, std::max(0, x0 + 1)), y1 = std::min(src.h - 1, std::max(0, y0 + 1));
                x0 = std::min(src.w - 1, std::max(0, x0));
                y0 = std::min(src.h - 1, std::max(0, y0));
                for (int k = 0; k < ch; ++k) {
                    float p00, p10, p01, p11;
                    if (isByte) {
                        p00 = src.row(y0)[x0 * 4 + k]; p10 = src.row(y0)[x1 * 4 + k];
                        p01 = src.row(y1)[x0 * 4 + k]; p11 = src.row(y1)[x1 * 4 + k];
                    } else {
                        p00 = ((const float*)src.row(y0))[x0 * 4 + k]; p10 = ((const float*)src.row(y0))[x1 * 4 + k];
                        p01 = ((const float*)src.row(y1))[x0 * 4 + k]; p11 = ((const float*)src.row(y1))[x1 * 4 + k];
                    }
                    acc[k] = (p00 * (1 - tx) + p10 * tx) * (1 - ty) + (p01 * (1 - tx) + p11 * tx) * ty;
                }
            }
            if (isByte) {
                uint8_t* d = out->row(oy + y) + (ox + x) * 4;
                for (int k = 0; k < ch; ++k) d[k] = (uint8_t)std::min(255.f, acc[k] + 0.5f);
            } else {
                float* d = (float*)out->row(oy + y) + (ox + x) * 4;
                for (int k = 0; k < ch; ++k) d[k] = acc[k];
            }
        }
    }
    });
    return out;
}

}  // namespace reno
