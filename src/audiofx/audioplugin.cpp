#include "audioplugin.h"
#include "vegasdistortion.h"
#include "vegas_trkfx.h"
#include "vegaspack3.h"
#include "vegasfx_pack2.h"
#include "vegaspack1.h"
#include <algorithm>
#include <set>
#include <cstdlib>
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

Json AudioPluginDesc::toJson() const
{
    Json j = Json::object();
    j["format"] = format;
    j["path"] = path;
    j["uid"] = uid;
    j["name"] = name;
    j["vendor"] = vendor;
    j["inputs"] = numInputs;
    j["outputs"] = numOutputs;
    j["editor"] = hasEditor;
    j["instrument"] = isInstrument;
    if (bridged32) j["bridged32"] = true;
    return j;
}

AudioPluginDesc AudioPluginDesc::fromJson(const Json& j)
{
    AudioPluginDesc d;
    d.format = j.get("format").asString();
    d.path = j.get("path").asString();
    d.uid = j.get("uid").asString();
    d.name = j.get("name").asString();
    d.vendor = j.get("vendor").asString();
    d.numInputs = j.get("inputs").asInt(2);
    d.numOutputs = j.get("outputs").asInt(2);
    d.hasEditor = j.get("editor").asBool();
    d.isInstrument = j.get("instrument").asBool();
    d.bridged32 = j.get("bridged32").asBool();
    return d;
}

// A VST3 module: Name.vst3 itself (a DLL on Windows), or the .so inside a Name.vst3 bundle on Linux.
static bool isVst3Path(const std::string& path)
{
    if (endsWithNoCase(path, ".vst3")) return true;
    std::string p = toLower(path);
    for (char& c : p) if (c == '\\') c = '/';
    return p.find(".vst3/contents/") != std::string::npos;
}

std::string builtinUidForDx(const std::string& dxUid)
{
    const std::string pack1 = vegpack1::uidForDx(dxUid);       // the sfppack1 effects (vegaspack1.cpp)
    if (!pack1.empty()) return pack1;
    {
        const std::string p2 = pack2BuiltinForDx(toLower(dxUid));      // sfppack2 / sfresfilter effects (vegasfx_pack2.h)
        if (!p2.empty()) return p2;
    }
    const std::string trk = vegtrk::builtinUidForDx(dxUid);       // Volume, Pan, Track EQ... (vegas_trkfx.h)
    if (!trk.empty()) return trk;
    if (toLower(dxUid).find(vegdist::kClsid) != std::string::npos) return "reno.distortion";
    return vegpack3::builtinUidForDx(dxUid);        // Amplitude Modulation, Flange/Wah-wah, Gapper/Snipper, Smooth/Enhance, Vibrato
}

std::string dxUidForBuiltin(const std::string& builtinUid)
{
    const std::string pack1 = vegpack1::dxForUid(builtinUid);
    if (!pack1.empty()) return pack1;
    {
        const std::string p2 = pack2DxForBuiltin(builtinUid);
        if (!p2.empty()) return p2;
    }
    const std::string trk = vegtrk::dxUidForBuiltin(builtinUid);
    if (!trk.empty()) return trk;
    if (builtinUid == "reno.distortion") return vegdist::kDxUid;
    const vegpack3::Entry* e = vegpack3::entryForBuiltin(builtinUid);
    return e ? std::string(e->dxUid) : std::string();
}

std::string builtinPageUid(const AudioPluginDesc& desc)
{
    if (desc.format == "BUILTIN") return desc.uid;
    if (desc.format == "DX" && desc.vendor == kBuiltinAudioVendor) return builtinUidForDx(desc.uid);
    return std::string();
}

std::vector<AudioPluginDesc> describeAudioPluginFile(const std::string& path, std::string* error)
{
    if (isVst3Path(path)) return vst3Describe(path, error);
    if (vst32Wanted(path)) return vst32Describe(path, error);
    return vst2Describe(path, error);
}

std::unique_ptr<AudioPlugin> createAudioPlugin(const AudioPluginDesc& desc, std::string* error)
{
    if (desc.format == "VST2" && efxPreferBuiltin() && efxReproducedId(desc.uid)) return efxCreate(desc, error);
    if (desc.format == "VST2") return (desc.bridged32 || vst32Wanted(desc.path)) ? vst32Create(desc, error) : vst2Create(desc, error);
    if (desc.format == "VST3") return vst3Create(desc, error);
    if (desc.format == "DX") {
        const std::string mine = builtinUidForDx(desc.uid);
        if (mine.empty()) return dxCreate(desc, error);
        std::string dxError;
        if (auto p = dxCreate(desc, &dxError)) return p;
        AudioPluginDesc b = desc;
        b.uid = mine;
        auto p = builtinAudioCreate(b, error);
        if (p) p->desc = desc, p->desc.vendor = kBuiltinAudioVendor;
        return p;
    }
    if (desc.format == "BUILTIN") return builtinAudioCreate(desc, error);
    if (error) *error = "unknown kind of audio plug-in: " + desc.format;
    return nullptr;
}

#ifdef _WIN32
// A text value of a registry key ("" if there is none).
static std::string regText(HKEY root, const std::wstring& key, const wchar_t* name, REGSAM view)
{
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return std::string();
    std::vector<wchar_t> buf(4096, 0);
    DWORD size = (DWORD)((buf.size() - 2) * sizeof(wchar_t)), type = 0;
    std::string out;
    if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf.data(), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)) {
        buf[buf.size() - 1] = buf[buf.size() - 2] = 0;
        // REG_MULTI_SZ: texts separated by 0, joined with ';'
        const size_t n = std::min<size_t>(size / sizeof(wchar_t), buf.size() - 2);
        for (size_t i = 0; i < n; ++i) if (buf[i] == 0 && i + 1 < n && buf[i + 1] != 0) buf[i] = L';';
        out = narrow(buf.data());
    }
    RegCloseKey(k);
    return out;
}

static std::vector<std::wstring> regSubkeys(HKEY root, const std::wstring& key, REGSAM view)
{
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0; i < 512; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(std::wstring(name, len));
    }
    RegCloseKey(k);
    return out;
}

std::vector<std::string> vegasPluginDirs()
{
    std::vector<std::string> out;
    auto add = [&](const std::string& d) {
        if (d.empty() || !dirExists(d)) return;
        for (const auto& e : out) if (toLower(e) == toLower(d)) return;
        out.push_back(d);
    };
    // Where VEGAS Pro is installed: its folder under Program Files ("VEGAS\VEGAS Pro 18.0" for VEGAS 14 and later;
    // MAGIX's and Sony's older ones), and where Windows was told vegas*.exe is (App Paths)
    std::vector<std::string> installs;
    wchar_t env[2048];
    for (const wchar_t* var : {L"ProgramW6432", L"ProgramFiles"}) {
        DWORD n = GetEnvironmentVariableW(var, env, 2048);
        if (n == 0 || n >= 2048) continue;
        const std::string pf = narrow(env);
        for (const char* company : {"VEGAS", "MAGIX", "Sony"}) {
            const std::string c = pathJoin(pf, company);
            for (const auto& name : listDir(c))
                if (toLower(name).compare(0, 5, "vegas") == 0) installs.push_back(pathJoin(c, name));
        }
    }
    const std::wstring appPaths = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    for (const auto& sub : regSubkeys(HKEY_LOCAL_MACHINE, appPaths, KEY_WOW64_64KEY)) {
        const std::string n = toLower(narrow(sub));
        if (n.compare(0, 5, "vegas") != 0 || !endsWithNoCase(n, ".exe")) continue;
        std::string dir = regText(HKEY_LOCAL_MACHINE, appPaths + L"\\" + sub, L"Path", KEY_WOW64_64KEY);
        if (dir.empty()) dir = pathDir(regText(HKEY_LOCAL_MACHINE, appPaths + L"\\" + sub, nullptr, KEY_WOW64_64KEY));
        while (!dir.empty() && (dir.back() == ';' || dir.back() == '\\' || dir.back() == '"')) dir.pop_back();
        if (!dir.empty() && dir[0] == '"') dir.erase(0, 1);
        if (!dir.empty()) installs.push_back(dir);
    }
    // VEGAS looks for VST effects in "MAGIX Plugins" beside itself (eFX effects: MAGIX Plugins\essentialFX\x64).
    for (const auto& i : installs) add(pathJoin(i, "MAGIX Plugins"));
    // Folders of Preferences > VST Effects: the default and two others, stored as "SearchPaths" under Software\<company>\<VEGAS Pro>\<version>\...
    // (';' between folders)
    for (const wchar_t* company : {L"VEGAS Creative Software", L"MAGIX", L"Sony Creative Software"}) {
        const std::wstring base = std::wstring(L"Software\\") + company;
        for (const auto& product : regSubkeys(HKEY_CURRENT_USER, base, 0)) {
            if (toLower(narrow(product)).find("vegas") == std::string::npos) continue;
            // searched a few keys deep: the version, then VEGAS's own sections
            std::vector<std::pair<std::wstring, int>> todo = {{base + L"\\" + product, 0}};
            for (size_t t = 0; t < todo.size() && t < 4000; ++t) {
                const std::wstring key = todo[t].first;
                const std::string paths = regText(HKEY_CURRENT_USER, key, L"SearchPaths", 0);
                size_t at = 0;
                while (at <= paths.size()) {
                    size_t end = paths.find(';', at);
                    if (end == std::string::npos) end = paths.size();
                    std::string d = paths.substr(at, end - at);
                    while (!d.empty() && (d.back() == ' ' || d.back() == '\\')) d.pop_back();
                    add(d);
                    at = end + 1;
                }
                if (todo[t].second < 3)
                    for (const auto& sub : regSubkeys(HKEY_CURRENT_USER, key, 0)) todo.push_back({key + L"\\" + sub, todo[t].second + 1});
            }
        }
    }
    return out;
}
#else
std::vector<std::string> vegasPluginDirs() { return {}; }
#endif

std::vector<std::string> audioPluginSearchDirs()
{
    std::vector<std::string> dirs;
    auto add = [&](const std::string& d) {
        if (d.empty() || !dirExists(d)) return;
        for (const auto& e : dirs) if (toLower(e) == toLower(d)) return;
        dirs.push_back(d);
    };
#ifdef _WIN32
    // the folder VST2 plug-ins register (used by VEGAS and installers), then the usual places
    // (both registry views: 32-bit installers write under WOW6432Node, KEY_WOW64_32KEY from here)
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VST", 0, KEY_READ | view, &key) == ERROR_SUCCESS) {
            wchar_t buf[2048];
            DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
            if (RegQueryValueExW(key, L"VSTPluginsPath", nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                buf[2047] = 0;
                std::wstring w = buf;
                if (type == REG_EXPAND_SZ) {
                    wchar_t ex[2048];
                    const DWORD n = ExpandEnvironmentStringsW(buf, ex, 2048);
                    if (n > 0 && n <= 2048) w = ex;
                }
                add(narrow(w));
            }
            RegCloseKey(key);
        }
    }
    wchar_t env[2048];
    std::vector<std::string> pf, cpf;
    // (also the 32-bit program folders, where 32-bit plug-ins are installed; run through reno_vst32)
    for (const wchar_t* var : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        DWORD n = GetEnvironmentVariableW(var, env, 2048);
        if (n > 0 && n < 2048) pf.push_back(narrow(env));
    }
    for (const wchar_t* var : {L"CommonProgramW6432", L"CommonProgramFiles", L"CommonProgramFiles(x86)"}) {
        DWORD n = GetEnvironmentVariableW(var, env, 2048);
        if (n > 0 && n < 2048) cpf.push_back(narrow(env));
    }
    for (const auto& p : pf) {
        add(pathJoin(p, "VSTPlugins"));
        add(pathJoin(p, "Steinberg\\VSTPlugins"));
        add(pathJoin(p, "Vstplugins"));
    }
    for (const auto& c : cpf) {
        add(pathJoin(c, "VST2"));
        add(pathJoin(c, "Steinberg\\VST2"));
        add(pathJoin(c, "VST3"));
    }
    // VEGAS's own VST plug-ins (eFX) and the folders VEGAS searches
    for (const auto& d : vegasPluginDirs()) add(d);
#else
    const char* home = std::getenv("HOME");
    if (home) { add(pathJoin(home, ".vst")); add(pathJoin(home, ".vst3")); }
    add("/usr/lib/vst");
    add("/usr/local/lib/vst");
    add("/usr/lib/vst3");
    add("/usr/local/lib/vst3");
#endif
    return dirs;
}

std::string vstIdText(const std::string& uid)
{
    char* end = nullptr;
    const unsigned long v = std::strtoul(uid.c_str(), &end, 10);
    if (uid.empty() || !end || *end) return uid;
    std::string s;
    for (int k = 3; k >= 0; --k) {
        const char c = (char)((v >> (8 * k)) & 0xFF);
        if (c < 32 || c > 126) return uid;
        s.push_back(c);
    }
    return s;
}

std::string audioPluginFileHint(const AudioPluginDesc& desc)
{
    // VEGAS's own DirectX plug-ins (VEGAS Pro 18; same class ids since Sound Forge) and the DLL holding each,
    // from the DLLs in VEGAS's Shared Plug-Ins\Audio_x64 folder
    static const struct { const char* clsid; const char* dll; } kDx[] = {
        {"39224540-6f92-11d0-aebc-00a0c9053912", "sfppack3_x64.dll"},     // Distortion
        {"d6802ba0-a056-11d0-aebc-00a0c9053912", "sfppack3_x64.dll"},     // Vibrato
        {"7298a3e0-78ee-11d0-aebc-00a0c9053912", "sfppack1_x64.dll"},     // Multi-Tap Delay
        {"607682e0-6e21-11d0-aebc-00a0c9053912", "sfppack1_x64.dll"},     // Reverb
        {"ed1b4100-93be-11d0-aebc-00a0c9053912", "sfppack1_x64.dll"},     // Pitch Shift
        {"28d9f1e0-6ecc-11d0-aebc-00a0c9053912", "sfppack1_x64.dll"},     // Chorus
        {"54f29260-79b1-11d0-aebc-00a0c9053912", "sfppack1_x64.dll"},     // Simple Delay
        {"8b7226ee-4584-11d1-b4cb-00a0c9270a10", "sfppack2_x64.dll"},     // Graphic EQ
        {"b97c0f23-196d-11d1-b99b-00a0c9053912", "sfppack2_x64.dll"},     // Noise Gate
        {"a8448720-96fd-11d0-aebc-00a0c9053912", "sfppack2_x64.dll"},     // Parametric EQ
        {"23c9f225-40ec-11d2-9d36-00c04f8edc1e", "sftrkfx1_x64.dll"},     // Track Compressor
        {"8cb69a0a-10e8-11d2-9b89-00104b8d13c2", "sftrkfx1_x64.dll"},     // Track EQ
        {"869419dd-501f-11d3-8cdc-00c04f6b8e4c", "sftrkfx1_x64.dll"},     // Track Noise Gate
        {"a6a78627-d619-48bf-ad26-0c6b44b5c7d8", "sfresfilter_x64.dll"},  // Resonant Filter
        {"ee38ca88-d78e-4bfb-b05e-577892730c83", "sffrgpnv_x64.dll"},     // Volume
        {"eb6213db-08ff-4510-9f8d-3058b0ece4c6", "sffrgpnv_x64.dll"},     // Pan
        {"260df3e1-ac77-11d2-9e93-00c04f68be44", "sftrkfx1_x64.dll"},     // Dither
        {"00000000-0f56-11d2-9887-00a0c969725b", "sfxpfx2_x64.dll"},      // ExpressFX Chorus
        {"00000001-0f56-11d2-9887-00a0c969725b", "sfxpfx2_x64.dll"},      // ExpressFX Delay
        {"00000003-0f56-11d2-9887-00a0c969725b", "sfxpfx2_x64.dll"},      // ExpressFX Equalization
        {"00000002-0f56-11d2-9887-00a0c969725b", "sfxpfx1_x64.dll"},      // ExpressFX Distortion
        {"00000006-0f56-11d2-9887-00a0c969725b", "sfxpfx1_x64.dll"},      // ExpressFX Reverb
        {"00000004-0f56-11d2-9887-00a0c969725b", "sfxpfx1_x64.dll"},      // ExpressFX Flange/Wah-Wah
        {"00000007-0f56-11d2-9887-00a0c969725b", "sfxpfx1_x64.dll"},      // ExpressFX Stutter
        {"00000009-0f56-11d2-9887-00a0c969725b", "sfxpfx3_x64.dll"},      // ExpressFX Graphic EQ
    };
    if (desc.format == "DX") {
        const std::string u = toLower(desc.uid);
        for (const auto& d : kDx)
            if (u.find(d.clsid) != std::string::npos)
                return std::string(d.dll) + ", one of VEGAS's DirectX plug-ins (in VEGAS's \"Shared Plug-Ins\\Audio_x64\" folder, "
                       "registered when VEGAS is installed)";
        return std::string();
    }
    if (desc.format == "VST2") {
        // VEGAS's "eFX" effects: eFX_<Name>.dll in VEGAS's own MAGIX Plugins\essentialFX\x64 folder
        const std::string n = desc.name;
        if (n.size() > 4 && toLower(n.substr(0, 4)) == "efx ") {
            std::string file = "eFX_";
            for (char c : n.substr(4)) if (c != ' ') file.push_back(c);
            return file + ".dll, one of VEGAS's own VST effects (in \"MAGIX Plugins\\essentialFX\\x64\" in VEGAS's folder)";
        }
    }
    return std::string();
}

static void findAudioIn(const std::string& dir, int depth, std::vector<std::string>& out)
{
    if (depth > 6) return;
#ifdef _WIN32
    const char* vst2Ext = ".dll";
    const char* vst3Arch = "x86_64-win";
#else
    const char* vst2Ext = ".so";
    const char* vst3Arch = "x86_64-linux";
#endif
    for (const auto& name : listDir(dir)) {
        std::string p = pathJoin(dir, name);
        if (endsWithNoCase(name, ".vst3")) {
            if (dirExists(p)) {             // a VST3 bundle: Name.vst3/Contents/<arch>/Name.vst3 (or .so)
                std::string bin = pathJoin(pathJoin(p, "Contents"), vst3Arch);
                for (const auto& f : listDir(bin))
                    if (endsWithNoCase(f, ".vst3") || endsWithNoCase(f, ".so")) out.push_back(pathJoin(bin, f));
            } else out.push_back(p);
        } else if (dirExists(p)) {
            findAudioIn(p, depth + 1, out);
        } else if (endsWithNoCase(name, vst2Ext)) {
            out.push_back(p);
        }
    }
}

std::vector<std::string> findAudioPluginFiles(const std::vector<std::string>& dirs)
{
    std::vector<std::string> all, unique;
    for (const auto& d : dirs) findAudioIn(d, 0, all);
    std::set<std::string> seen;
    for (const auto& p : all)
        if (seen.insert(toLower(p)).second) unique.push_back(p);
    return unique;
}

}  // namespace reno
