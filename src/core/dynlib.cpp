#include "dynlib.h"
#include "base.h"
#include <mutex>
#include <vector>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace reno {

#ifdef _WIN32
static std::mutex gDirMutex;
static std::vector<std::string> gDirs;

void DynLib::addSearchDir(const std::string& dir)
{
    if (dir.empty() || !dirExists(dir)) return;
    std::lock_guard<std::mutex> lk(gDirMutex);
    for (const auto& d : gDirs) if (toLower(d) == toLower(dir)) return;
    gDirs.push_back(dir);
    // Both ways: AddDllDirectory for loads made with LOAD_LIBRARY_SEARCH_*, and PATH for everything else
    // (a plug-in that loads its own helpers by name later on).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(widen(dir).c_str());
    std::wstring path(32768, L'\0');
    DWORD n = GetEnvironmentVariableW(L"PATH", &path[0], (DWORD)path.size());
    path.resize(n < path.size() ? n : 0);
    std::wstring add = widen(dir);
    SetEnvironmentVariableW(L"PATH", (path.empty() ? add : path + L";" + add).c_str());
}

bool DynLib::open(const std::string& path, std::string* error)
{
    close();
    const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::wstring w = widen(path);
    for (auto& c : w) if (c == L'/') c = L'\\';
    HMODULE h = LoadLibraryExW(w.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    DWORD code = h ? 0 : GetLastError();
    if (!h) {
        h = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) code = GetLastError();
    }
    SetErrorMode(oldMode);
    if (!h) {
        if (error) {
            wchar_t* msg = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, (LPWSTR)&msg, 0, nullptr);
            std::string m = msg ? narrow(msg) : std::string();
            while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
            if (msg) LocalFree(msg);
            *error = strfmt("error %lu: %s", (unsigned long)code, m.c_str());
        }
        return false;
    }
    handle_ = (void*)h;
    return true;
}

void DynLib::close()
{
    if (handle_) FreeLibrary((HMODULE)handle_);
    handle_ = nullptr;
}

void* DynLib::symbol(const char* name) const
{
    return handle_ ? (void*)GetProcAddress((HMODULE)handle_, name) : nullptr;
}

#else

void DynLib::addSearchDir(const std::string&) {}

bool DynLib::open(const std::string& path, std::string* error)
{
    close();
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* e = dlerror();
        if (error) *error = e ? e : "dlopen failed";
        return false;
    }
    return true;
}

void DynLib::close()
{
    if (handle_) dlclose(handle_);
    handle_ = nullptr;
}

void* DynLib::symbol(const char* name) const
{
    return handle_ ? dlsym(handle_, name) : nullptr;
}

#endif

}  // namespace reno
