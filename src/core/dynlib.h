// Loading plug-in files (LoadLibrary / dlopen).
#pragma once
#include <string>

namespace reno {

class DynLib {
public:
    DynLib() {}
    ~DynLib() { close(); }
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;

    // Opens the file. Libraries the file needs are looked for next to it first, then in any folder given to addSearchDir.
    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    void* symbol(const char* name) const;
    bool isOpen() const { return handle_ != nullptr; }
    void leak() { handle_ = nullptr; }          // never unload (for plug-ins that crash when unloaded)
    void* nativeHandle() const { return handle_; }

    // Extra folders to look in for the libraries a plug-in needs (VEGAS's own effects need DLLs from its program folder).
    static void addSearchDir(const std::string& dir);

private:
    void* handle_ = nullptr;
};

}  // namespace reno
