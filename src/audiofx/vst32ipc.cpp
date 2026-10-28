#include "vst32ipc.h"
#include "../core/base.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#  include <errno.h>
#  include <fcntl.h>
#  include <semaphore.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <time.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace reno {
namespace vst32 {

namespace {
std::atomic<unsigned> gLinkCounter{0};

// Frames are written whole (one write per message, under a lock) and read as head, then payload.
std::string frameBytes(uint32_t op, uint32_t seq, const std::string& payload)
{
    FrameHead h;
    h.size = (uint32_t)payload.size();
    h.op = op;
    h.seq = seq;
    h.reserved = 0;
    std::string out((const char*)&h, sizeof(h));
    out += payload;
    return out;
}
}  // namespace

#ifdef _WIN32
// ================================================================================================ Windows
struct Endpoint::Impl {
    HANDLE map = nullptr, evReq = nullptr, evResp = nullptr, rd = nullptr, wr = nullptr, proc = nullptr;
    bool owner = false;             // Reno's end (it started the process)
    DWORD parentPid = 0;
    HANDLE parentProc = nullptr;    // (server) Reno's process, to see whether it is still there
};

static HANDLE jobForChildren()
{
    // The bridge processes are in a job that ends them when Reno ends, however it ends
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
            std::memset(&info, 0, sizeof(info));
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return j;
    }();
    return job;
}

Endpoint::Endpoint() : d_(new Impl()) {}
Endpoint::~Endpoint() { close(); delete d_; }

bool Endpoint::start(const std::string& exe, std::string* error)
{
    close();
    d_->owner = true;
    const std::string id = strfmt("reno_vst32_%lu_%u_%lu", (unsigned long)GetCurrentProcessId(), ++gLinkCounter, (unsigned long)GetTickCount());
    const std::wstring wid = widen(id);
    d_->map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)shmBytes(), (L"Local\\" + wid + L"_m").c_str());
    if (!d_->map) { if (error) *error = strfmt("could not make shared memory (error %lu)", GetLastError()); return false; }
    mem_ = MapViewOfFile(d_->map, FILE_MAP_ALL_ACCESS, 0, 0, shmBytes());
    d_->evReq = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\" + wid + L"_rq").c_str());
    d_->evResp = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\" + wid + L"_rs").c_str());
    if (!mem_ || !d_->evReq || !d_->evResp) { if (error) *error = "could not make the bridge's events"; close(); return false; }
    std::memset(mem_, 0, sizeof(ShmHeader));
    ShmHeader* h = header();
    h->magic = kShmMagic;
    h->version = kProtocolVersion;
    h->capacity = kBlockCapacity;
    h->maxParams = kMaxParams;

    // two anonymous pipes; the process gets one end of each, by handle value, and only those (handle list)
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE c2sR = nullptr, c2sW = nullptr, s2cR = nullptr, s2cW = nullptr;
    if (!CreatePipe(&c2sR, &c2sW, &sa, 1 << 20) || !CreatePipe(&s2cR, &s2cW, &sa, 1 << 20)) {
        if (error) *error = strfmt("could not make the bridge's pipes (error %lu)", GetLastError());
        for (HANDLE x : {c2sR, c2sW, s2cR, s2cW}) if (x) CloseHandle(x);
        close();
        return false;
    }
    SetHandleInformation(c2sW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(s2cR, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + widen(exe) + L"\" " + wid + L" " + std::to_wstring((unsigned long long)(uintptr_t)c2sR) + L" " +
                       std::to_wstring((unsigned long long)(uintptr_t)s2cW) + L" " + std::to_wstring((unsigned long)GetCurrentProcessId());
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrMem(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrMem.data();
    HANDLE inherit[2] = {c2sR, s2cW};
    bool attrsOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                   UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    STARTUPINFOEXW si;
    std::memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));
    const std::wstring dir = widen(pathDir(exe));
    BOOL ok = attrsOk && CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                        dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
    const DWORD code = ok ? 0 : GetLastError();
    if (attrsOk) DeleteProcThreadAttributeList(attrs);
    CloseHandle(c2sR);
    CloseHandle(s2cW);
    if (!ok) {
        if (error) *error = strfmt("could not start %s (error %lu)", pathFileName(exe).c_str(), (unsigned long)code);
        CloseHandle(c2sW);
        CloseHandle(s2cR);
        close();
        return false;
    }
    if (HANDLE job = jobForChildren()) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    d_->proc = pi.hProcess;
    d_->wr = c2sW;
    d_->rd = s2cR;
    return true;
}

bool Endpoint::attach(const std::vector<std::string>& args, std::string* error)
{
    close();
    if (args.size() < 4) { if (error) *error = "too few arguments"; return false; }
    const std::wstring wid = widen(args[0]);
    d_->rd = (HANDLE)(uintptr_t)std::strtoull(args[1].c_str(), nullptr, 10);
    d_->wr = (HANDLE)(uintptr_t)std::strtoull(args[2].c_str(), nullptr, 10);
    d_->parentPid = (DWORD)std::strtoul(args[3].c_str(), nullptr, 10);
    d_->map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (L"Local\\" + wid + L"_m").c_str());
    if (d_->map) mem_ = MapViewOfFile(d_->map, FILE_MAP_ALL_ACCESS, 0, 0, shmBytes());
    d_->evReq = OpenEventW(EVENT_ALL_ACCESS, FALSE, (L"Local\\" + wid + L"_rq").c_str());
    d_->evResp = OpenEventW(EVENT_ALL_ACCESS, FALSE, (L"Local\\" + wid + L"_rs").c_str());
    d_->parentProc = OpenProcess(SYNCHRONIZE, FALSE, d_->parentPid);
    if (!mem_ || !d_->evReq || !d_->evResp) { if (error) *error = "could not open the shared memory and events"; return false; }
    return true;
}

void Endpoint::kill()
{
    if (d_->proc) {
        TerminateProcess(d_->proc, 1);
        WaitForSingleObject(d_->proc, 2000);
    }
}

bool Endpoint::alive()
{
    if (d_->owner) return d_->proc && WaitForSingleObject(d_->proc, 0) == WAIT_TIMEOUT;
    return !d_->parentProc || WaitForSingleObject(d_->parentProc, 0) == WAIT_TIMEOUT;
}

long long Endpoint::processId() const { return d_->proc ? (long long)GetProcessId(d_->proc) : 0; }

void Endpoint::close()
{
    if (mem_) UnmapViewOfFile(mem_);
    mem_ = nullptr;
    for (HANDLE* h : {&d_->map, &d_->evReq, &d_->evResp, &d_->rd, &d_->wr, &d_->proc, &d_->parentProc})
        if (*h) { CloseHandle(*h); *h = nullptr; }
}

bool Endpoint::send(uint32_t op, uint32_t seq, const std::string& payload)
{
    const std::string bytes = frameBytes(op, seq, payload);
    std::lock_guard<std::mutex> lk(writeMutex_);
    if (!d_->wr) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD n = 0;
        if (!WriteFile(d_->wr, bytes.data() + done, (DWORD)std::min<size_t>(bytes.size() - done, 1 << 20), &n, nullptr) || n == 0) return false;
        done += n;
    }
    return true;
}

static bool readAll(HANDLE h, void* out, size_t n)
{
    size_t done = 0;
    while (done < n) {
        DWORD got = 0;
        if (!ReadFile(h, (char*)out + done, (DWORD)std::min<size_t>(n - done, 1 << 20), &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

bool Endpoint::read(FrameHead& head, std::string& payload)
{
    if (!d_->rd || !readAll(d_->rd, &head, sizeof(head))) return false;
    if (head.size > kMaxPayload) return false;
    payload.resize(head.size);
    return head.size == 0 || readAll(d_->rd, &payload[0], head.size);
}

void Endpoint::postRequest() { SetEvent(d_->evReq); }
void Endpoint::postResponse() { SetEvent(d_->evResp); }

bool Endpoint::waitRequest(int ms)
{
    // (server) also ends if Reno has gone
    HANDLE hs[2] = {d_->evReq, d_->parentProc};
    const DWORD r = WaitForMultipleObjects(d_->parentProc ? 2 : 1, hs, FALSE, ms < 0 ? INFINITE : (DWORD)ms);
    if (r == WAIT_OBJECT_0 + 1) { header()->quit.store(1); return true; }
    return r == WAIT_OBJECT_0;
}

int Endpoint::waitResponse(int ms)
{
    HANDLE hs[2] = {d_->evResp, d_->proc};
    const DWORD r = WaitForMultipleObjects(2, hs, FALSE, ms < 0 ? INFINITE : (DWORD)ms);
    if (r == WAIT_OBJECT_0) return 1;
    if (r == WAIT_OBJECT_0 + 1) return WaitForSingleObject(d_->evResp, 0) == WAIT_OBJECT_0 ? 1 : -1;
    return 0;
}

#else
// ================================================================================================ POSIX (Linux)
struct Endpoint::Impl {
    int shmFd = -1, rd = -1, wr = -1;
    pid_t pid = 0;
    bool owner = false, reaped = false;
};

// The descriptors the process gets (fixed numbers, so the server finds them)
static const int kFdShm = 200, kFdIn = 201, kFdOut = 202;

// A pipe whose ends are not inherited by programs started later (pipe2 where there is one; macOS has only pipe)
static bool pipeCloexec(int fds[2])
{
#ifdef __APPLE__
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#else
    return pipe2(fds, O_CLOEXEC) == 0;
#endif
}

Endpoint::Endpoint() : d_(new Impl()) {}
Endpoint::~Endpoint() { close(); delete d_; }

bool Endpoint::start(const std::string& exe, std::string* error)
{
    close();
#ifdef __APPLE__
    // macOS runs no 32-bit programs at all (and has no process-shared unnamed semaphores): there is no bridge to start
    (void)exe;
    if (error) *error = "32-bit plug-ins cannot run on macOS";
    return false;
#endif
    d_->owner = true;
    d_->reaped = false;
    signal(SIGPIPE, SIG_IGN);       // a write to a closed pipe is an error to handle, not a reason to exit
    const std::string name = strfmt("/reno_vst32_%ld_%u", (long)getpid(), ++gLinkCounter);
    d_->shmFd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (d_->shmFd < 0) { if (error) *error = strfmt("could not make shared memory (%s)", std::strerror(errno)); return false; }
    shm_unlink(name.c_str());
    if (ftruncate(d_->shmFd, (off_t)shmBytes()) != 0) { if (error) *error = "could not size shared memory"; close(); return false; }
    mem_ = mmap(nullptr, shmBytes(), PROT_READ | PROT_WRITE, MAP_SHARED, d_->shmFd, 0);
    if (mem_ == MAP_FAILED) { mem_ = nullptr; if (error) *error = "could not map shared memory"; close(); return false; }
    std::memset(mem_, 0, sizeof(ShmHeader));
    ShmHeader* h = header();
    h->magic = kShmMagic;
    h->version = kProtocolVersion;
    h->capacity = kBlockCapacity;
    h->maxParams = kMaxParams;
    sem_init((sem_t*)h->sems[0], 1, 0);
    sem_init((sem_t*)h->sems[1], 1, 0);

    int c2s[2], s2c[2];
    if (!pipeCloexec(c2s)) { if (error) *error = "could not make a pipe"; close(); return false; }
    if (!pipeCloexec(s2c)) { ::close(c2s[0]); ::close(c2s[1]); if (error) *error = "could not make a pipe"; close(); return false; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, d_->shmFd, kFdShm);
    posix_spawn_file_actions_adddup2(&fa, c2s[0], kFdIn);
    posix_spawn_file_actions_adddup2(&fa, s2c[1], kFdOut);
    const std::string a1 = "linux", a2 = std::to_string((long)getpid());
    char* argv[] = {const_cast<char*>(exe.c_str()), const_cast<char*>(a1.c_str()), const_cast<char*>(a2.c_str()), nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(c2s[0]);
    ::close(s2c[1]);
    if (rc != 0) {
        if (error) *error = strfmt("could not start %s (%s)", pathFileName(exe).c_str(), std::strerror(rc));
        ::close(c2s[1]);
        ::close(s2c[0]);
        close();
        return false;
    }
    d_->pid = pid;
    d_->wr = c2s[1];
    d_->rd = s2c[0];
    return true;
}

bool Endpoint::attach(const std::vector<std::string>& args, std::string* error)
{
    close();
    signal(SIGPIPE, SIG_IGN);
    d_->shmFd = kFdShm;
    d_->rd = kFdIn;
    d_->wr = kFdOut;
    d_->pid = args.size() > 1 ? (pid_t)std::atol(args[1].c_str()) : 0;     // Reno's process id
    mem_ = mmap(nullptr, shmBytes(), PROT_READ | PROT_WRITE, MAP_SHARED, d_->shmFd, 0);
    if (mem_ == MAP_FAILED) { mem_ = nullptr; if (error) *error = "could not map the shared memory"; return false; }
    return true;
}

void Endpoint::kill()
{
    if (d_->owner && d_->pid > 0 && !d_->reaped) {
        ::kill(d_->pid, SIGKILL);
        int st = 0;
        waitpid(d_->pid, &st, 0);
        d_->reaped = true;
    }
}

bool Endpoint::alive()
{
    if (d_->owner) {
        if (d_->pid <= 0 || d_->reaped) return false;
        int st = 0;
        const pid_t r = waitpid(d_->pid, &st, WNOHANG);
        if (r == d_->pid || (r < 0 && errno == ECHILD)) { d_->reaped = true; return false; }
        return true;
    }
    return d_->pid <= 0 || getppid() == d_->pid;
}

long long Endpoint::processId() const { return d_->owner ? (long long)d_->pid : 0; }

void Endpoint::close()
{
    if (mem_) munmap(mem_, shmBytes());
    mem_ = nullptr;
    for (int* fd : {&d_->shmFd, &d_->rd, &d_->wr})
        if (*fd >= 0) { ::close(*fd); *fd = -1; }
}

bool Endpoint::send(uint32_t op, uint32_t seq, const std::string& payload)
{
    const std::string bytes = frameBytes(op, seq, payload);
    std::lock_guard<std::mutex> lk(writeMutex_);
    if (d_->wr < 0) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(d_->wr, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;       // a closed pipe gives EPIPE; SIGPIPE is ignored
        done += (size_t)n;
    }
    return true;
}

static bool readAll(int fd, void* out, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t got = ::read(fd, (char*)out + done, n - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += (size_t)got;
    }
    return true;
}

bool Endpoint::read(FrameHead& head, std::string& payload)
{
    if (d_->rd < 0 || !readAll(d_->rd, &head, sizeof(head))) return false;
    if (head.size > kMaxPayload) return false;
    payload.resize(head.size);
    return head.size == 0 || readAll(d_->rd, &payload[0], head.size);
}

void Endpoint::postRequest() { sem_post((sem_t*)header()->sems[0]); }
void Endpoint::postResponse() { sem_post((sem_t*)header()->sems[1]); }

static bool semWait(sem_t* s, int ms)
{
    if (ms < 0) {
        while (sem_wait(s) != 0) if (errno != EINTR) return false;
        return true;
    }
    if (sem_trywait(s) == 0) return true;
    if (ms == 0) return false;
#ifdef __APPLE__
    // (no sem_timedwait on macOS: tried every millisecond until the time is up)
    for (int waited = 0; waited < ms; ++waited) {
        usleep(1000);
        if (sem_trywait(s) == 0) return true;
    }
    return false;
#else
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    for (;;) {
        if (sem_timedwait(s, &ts) == 0) return true;
        if (errno != EINTR) return false;
    }
#endif
}

bool Endpoint::waitRequest(int ms)
{
    // (server) in slices, to notice Reno going away
    sem_t* s = (sem_t*)header()->sems[0];
    int left = ms;
    for (;;) {
        const int slice = (left < 0 || left > 200) ? 200 : left;
        if (semWait(s, slice)) return true;
        if (!alive()) { header()->quit.store(1); return true; }
        if (left >= 0) { left -= slice; if (left <= 0) return false; }
    }
}

int Endpoint::waitResponse(int ms)
{
    sem_t* s = (sem_t*)header()->sems[1];
    int left = ms;
    for (;;) {
        const int slice = (left < 0 || left > 20) ? 20 : left;
        if (semWait(s, slice)) return 1;
        if (!alive()) return semWait(s, 0) ? 1 : -1;
        if (left >= 0) { left -= slice; if (left <= 0) return 0; }
    }
}
#endif

}  // namespace vst32
}  // namespace reno
