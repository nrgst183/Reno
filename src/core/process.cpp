#include "process.h"
#include "base.h"
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#  include <signal.h>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <climits>
#  include <cstdlib>
#  include <chrono>
#  include <thread>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
#endif

namespace reno {

bool ProcessResult::crashed() const
{
#ifdef _WIN32
    return started && !timedOut && !stopped && ((unsigned long long)exitCode & 0xF0000000ull) == 0xC0000000ull;
#else
    return started && !timedOut && !stopped && exitCode >= 128;
#endif
}

std::string ProcessResult::describe() const
{
    if (!started) return "could not be started";
    if (timedOut) return "took too long and was stopped";
    if (stopped) return "was stopped";
    if (crashed()) {
#ifdef _WIN32
        const unsigned long code = (unsigned long)exitCode;
        const char* what = code == 0xC0000005ul ? "access violation" : code == 0xC00000FDul ? "stack overflow" :
                           code == 0xC0000409ul ? "stack buffer overrun / fail-fast" : code == 0xC0000374ul ? "heap corruption" :
                           code == 0xC000001Dul ? "illegal instruction" : code == 0xC0000135ul ? "a DLL it needs was not found" :
                           code == 0xC0000139ul ? "a function it needs was not found in a DLL" : "exception";
        return strfmt("crashed (%s, 0x%08lX)", what, code);
#else
        return strfmt("crashed (signal %lld)", exitCode - 128);
#endif
    }
    return strfmt("exit code %lld", exitCode);
}

#ifdef _WIN32

std::string selfExePath()
{
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return narrow(std::wstring(buf, n));
}

void quietCrashes()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

// One argument, quoted the way CommandLineToArgvW undoes.
static std::wstring quoteArg(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); slashes = 0; continue; }
        out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

ProcessResult runProcess(const std::string& exe, const std::vector<std::string>& args, int timeoutSeconds, const std::atomic<bool>* stop)
{
    ProcessResult r;
    std::wstring cmd = quoteArg(widen(exe));
    for (const auto& a : args) { cmd.push_back(L' '); cmd += quoteArg(widen(a)); }
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(widen(exe).c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return r;
    r.started = true;
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 50) != WAIT_TIMEOUT) break;
        if (stop && stop->load()) {
            r.stopped = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        if (timeoutSeconds > 0 && GetTickCount64() - start > (ULONGLONG)timeoutSeconds * 1000) {
            r.timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = (long long)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

#else

std::string selfExePath()
{
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return std::string();
    char real[PATH_MAX];
    return realpath(buf, real) ? std::string(real) : std::string(buf);
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
#endif
}

void quietCrashes() {}

ProcessResult runProcess(const std::string& exe, const std::vector<std::string>& args, int timeoutSeconds, const std::atomic<bool>* stop)
{
    ProcessResult r;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) return r;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    r.started = true;
    const auto start = std::chrono::steady_clock::now();
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) return r;
        if (stop && stop->load()) {
            r.stopped = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        if (timeoutSeconds > 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds)) {
            r.timedOut = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (WIFEXITED(status)) r.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.exitCode = 128 + WTERMSIG(status);
    return r;
}

#endif

}  // namespace reno
