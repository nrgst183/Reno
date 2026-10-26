// vegaudiofx_test: every audio effect in the sample VEGAS projects, from the .veg to the plug-in.
//
// On Linux (the importer):
//   vegaudiofx_test [folder with .veg files] [--list out.txt]      (default $RENO_SAMPLES)
// For every audio effect chain in every project (events, tracks, buses, the master; nested projects too) the chains
// are read straight from the file's bytes, independently of the importer, and the importer's effect slots must match
// one for one: name, kind and id, place in the chain, bypass, and saved settings exactly as the plug-in saved them:
//   - a DirectX plug-in's slot holds everything its IPersistStream::Save wrote ("S:"), first byte to last (VEGAS's own
//     plug-ins start with a length);
//   - a VST plug-in's slot holds its program: "P:" one value per parameter (FxCk), or "Q:" the plug-in's chunk of
//     that program (FPCh, loaded with effSetChunk's "this is one program");
// and saving the project writes the effect back as it was. --list writes every distinct effect
// (format|uid|name|state|bypass) for the Windows part.
//
// On Windows (the plug-in hosts; built with the Windows cross compiler and run under Wine with VEGAS's plug-ins
// installed in the Wine prefix):
//   vegaudiofx_test --plugins list.txt [folder for pictures of the plug-ins' own windows]
// For every effect in the list: it is found (VST: in the folders Reno looks in, which include VEGAS's own), made, given
// its settings, started and run; what it saves must equal what it was given (DirectX), or its parameters must be the
// given values (VST). With a folder, each DirectX plug-in's window (property page) is opened after the settings are
// loaded and saved as a picture.
#include "../../src/audiofx/audioplugin.h"
#include "../../src/core/base.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../testdata.h"
#ifndef _WIN32
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/vegas/vegorigin.h"
#include <filesystem>
#include <functional>
#include <algorithm>
#else
#include <windows.h>
#endif

using namespace reno;

static int g_fail = 0, g_pass = 0;

static void check(bool ok, const std::string& what)
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what.c_str()); }
}

static uint32_t le32(const std::string& b, size_t at)
{
    return (uint32_t)(uint8_t)b[at] | ((uint32_t)(uint8_t)b[at + 1] << 8) | ((uint32_t)(uint8_t)b[at + 2] << 16) | ((uint32_t)(uint8_t)b[at + 3] << 24);
}
static uint32_t be32(const std::string& b, size_t at)
{
    return ((uint32_t)(uint8_t)b[at] << 24) | ((uint32_t)(uint8_t)b[at + 1] << 16) | ((uint32_t)(uint8_t)b[at + 2] << 8) | (uint32_t)(uint8_t)b[at + 3];
}

#ifndef _WIN32
// ------------------------------------------------------------------------------------------------ Linux

// One plug-in of a chain as the file has it (read here, not by the importer).
struct RawPlug {
    std::string clsid;              // as text, lower case
    uint32_t flags = 0;
    std::string name;
    std::string data;               // the bytes after the size
    uint32_t trailer = 0;
};

static std::string guidText(const std::string& b, size_t at)
{
    return strfmt("%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", le32(b, at), (unsigned)(uint8_t)b[at + 4] | ((unsigned)(uint8_t)b[at + 5] << 8),
                  (unsigned)(uint8_t)b[at + 6] | ((unsigned)(uint8_t)b[at + 7] << 8), (uint8_t)b[at + 8], (uint8_t)b[at + 9], (uint8_t)b[at + 10],
                  (uint8_t)b[at + 11], (uint8_t)b[at + 12], (uint8_t)b[at + 13], (uint8_t)b[at + 14], (uint8_t)b[at + 15]);
}

static std::string utf16(const std::string& b, size_t at, size_t bytes)
{
    std::string s;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        const unsigned c = (uint8_t)b[at + i] | ((unsigned)(uint8_t)b[at + i + 1] << 8);
        if (!c) break;
        if (c < 0x80) s.push_back((char)c);
        else if (c < 0x800) { s.push_back((char)(0xC0 | (c >> 6))); s.push_back((char)(0x80 | (c & 0x3F))); }
        else { s.push_back((char)(0xE0 | (c >> 12))); s.push_back((char)(0x80 | ((c >> 6) & 0x3F))); s.push_back((char)(0x80 | (c & 0x3F))); }
    }
    return s;
}

// Every chain in the file (G::AudioChain, wherever it is), plug-in by plug-in.
static std::vector<RawPlug> rawPlugins(const std::string& b)
{
    static const char kChain[16] = {0x01, (char)0x86, 0x12, (char)0xb4, 0x2e, 0x70, (char)0xd1, 0x11, (char)0x82, 0x53, 0x00, 0x00, (char)0x92, (char)0x90, (char)0xdd, 0x44};
    std::vector<RawPlug> out;
    for (size_t at = b.find(std::string(kChain, 16)); at != std::string::npos; at = b.find(std::string(kChain, 16), at + 16)) {
        if (at + 40 > b.size()) break;
        size_t p = at + 40 + le32(b, at + 36) + 8 + 18 + 4;
        if (p + 8 > b.size()) continue;
        const uint32_t n = le32(b, p);
        p += 8;
        for (uint32_t i = 0; i < n && n < 1000; ++i) {
            if (p + 24 > b.size()) break;
            RawPlug r;
            r.clsid = guidText(b, p);
            r.flags = le32(b, p + 16);
            const uint32_t nl = le32(b, p + 20);
            if (p + 24 + nl + 20 > b.size()) break;
            r.name = utf16(b, p + 24, nl);
            const size_t q = p + 24 + nl + 16;
            const uint32_t dl = le32(b, q);
            if (q + 4 + dl + 4 > b.size()) break;
            r.data = b.substr(q + 4, dl);
            r.trailer = le32(b, q + 4 + dl);
            out.push_back(r);
            p = q + 4 + dl + 4;
        }
    }
    return out;
}

static bool isVst(const RawPlug& r) { return r.clsid.size() == 36 && r.clsid.substr(19) == "0269-0007e95d36fa"; }

// What Reno's slot must hold for this plug-in, worked out here from the bytes.
static std::string expectedState(const RawPlug& r, std::string& uid, std::string& format)
{
    if (!isVst(r)) {
        format = "DX";
        std::string g = r.clsid;
        for (auto& c : g) c = (char)std::toupper((unsigned char)c);
        uid = "dshow:{" + g + "}";
        return "S:" + base64Encode(r.data.data(), r.data.size());
    }
    format = "VST2";
    const std::string prog = r.data.substr(4);
    if (prog.size() < 60 || prog.compare(0, 4, "CcnK") != 0) return std::string();
    uid = strfmt("%u", be32(prog, 16));
    if (prog.compare(8, 4, "FxCk") == 0) {
        const uint32_t n = be32(prog, 24);
        std::vector<float> v(n);
        for (uint32_t i = 0; i < n && 56 + 4 * (size_t)i + 4 <= prog.size(); ++i) {
            const uint32_t x = be32(prog, 56 + 4 * (size_t)i);
            std::memcpy(&v[i], &x, 4);
        }
        return "P:" + base64Encode(v.data(), v.size() * 4);
    }
    if (prog.compare(8, 4, "FPCh") == 0) {
        const uint32_t n = be32(prog, 56);
        return "Q:" + base64Encode(prog.data() + 60, std::min<size_t>(n, prog.size() - 60));
    }
    return std::string();
}

// The project's own path, as VEGAS wrote it into the file (what the writer is told when it saves the project back).
static std::string headerPath(const std::string& bytes)
{
    veg::ChunkTree t;
    veg::readChunkTree(veg::Bytes((const uint8_t*)bytes.data(), bytes.size()), t);
    for (const auto& k : t.root.kids)
        if (!k.isList && k.id == veg::G::Header) return k.data.textZ(k.data.u32(0));
    return std::string();
}

static void eachSlot(Project& p, const std::function<void(FxSlot&, const std::string&)>& f)
{
    for (auto& t : p.tracks) {
        for (auto& s : t.fx) f(s, "track");
        for (auto& e : t.events)
            for (auto& s : e.fx) f(s, "event");
    }
    for (auto& b : p.buses)
        for (auto& s : b.fx) f(s, "bus");
    for (auto& s : p.audioOut) f(s, "master");
}

static std::string slotKey(const std::string& name, const std::string& format, const std::string& uid, bool bypass, const std::string& state)
{
    return name + "|" + format + "|" + uid + "|" + (bypass ? "off" : "on") + "|" + state;
}

int main(int argc, char** argv)
{
    std::string root = testDataDir("RENO_SAMPLES"), listOut, chunkVst;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list") && i + 1 < argc) listOut = argv[++i];
        else if (!std::strcmp(argv[i], "--chunk-vst") && i + 1 < argc) chunkVst = argv[++i];
        else root = argv[i];
    }
    if (testDataMissing(root)) return 0;
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
        if (e.is_regular_file() && endsWithNoCase(e.path().string(), ".veg")) files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    check(!files.empty(), "there are projects in " + root);

    size_t total = 0, dx = 0, vst = 0, off = 0, withPrefix = 0, chunked = 0, params = 0;
    std::map<std::string, std::string> distinct;        // key -> list line
    std::map<std::string, int> perPlugin;
    for (const auto& f : files) {
        const std::string name = pathFileName(f);
        std::string bytes;
        readFile(f, bytes);
        const std::vector<RawPlug> raw = rawPlugins(bytes);
        Project p;
        const VegasImportResult r = importVegasData(bytes, f, p);
        check(r.ok, name + ": opens (" + r.error + ")");
        if (!r.ok) continue;

        // the importer's slots against the bytes: the same plug-ins, the same settings, nothing more or less
        std::multiset<std::string> want, got;
        for (const auto& x : raw) {
            std::string uid, format;
            const std::string st = expectedState(x, uid, format);
            check(!st.empty(), name + ": '" + x.name + "' has settings this test can read");
            want.insert(slotKey(x.name, format, uid, (x.flags & 1) != 0, st));
            ++total;
            perPlugin[x.name.empty() ? x.clsid : x.name]++;
            if (format == "DX") {
                ++dx;
                // VEGAS's own DirectX plug-ins save their length first: all of them in the samples
                if (x.data.size() >= 4 && le32(x.data, 0) == x.data.size()) ++withPrefix;
            } else {
                ++vst;
                if (st.compare(0, 2, "Q:") == 0) ++chunked;
                if (st.compare(0, 2, "P:") == 0) ++params;
            }
            if (x.flags & 1) ++off;
            check(x.flags <= 1, name + ": '" + x.name + "' has only the switched-off flag (" + strfmt("%u", x.flags) + ")");
            distinct[format + "|" + uid + "|" + st] = format + "|" + uid + "|" + x.name + "|" + st + "|" + ((x.flags & 1) ? "1" : "0");
        }
        // (the importer's chains in file order: events of a track before the track's own chain is not the file's
        // order, so the two are compared as sets of whole entries, and each chain's order below)
        eachSlot(p, [&](FxSlot& s, const std::string&) {
            if (s.kind != "audio") return;
            got.insert(slotKey(s.name, s.audio.format, s.audio.uid, s.bypass, s.state.isString() ? s.state.asString() : std::string()));
        });
        check(want == got, name + strfmt(": the importer's %zu audio effects are the file's %zu, with their settings and switches", got.size(), want.size()));
        if (want != got) {
            for (const auto& w : want) if (!got.count(w)) std::printf("   file only:     %.160s\n", w.c_str());
            for (const auto& g : got) if (!want.count(g)) std::printf("   importer only: %.160s\n", g.c_str());
        }
        // each chain keeps its order: the slots of every chain, in order, appear in the file in that order
        size_t from = 0;
        bool ordered = true;
        auto chainOrder = [&](const FxChain& c) {
            std::vector<std::string> names;
            for (const auto& s : c) if (s.kind == "audio") names.push_back(s.name + "|" + (s.state.isString() ? s.state.asString() : ""));
            if (names.size() < 2) return;
            // find the run of raw entries equal to this chain
            bool found = false;
            for (size_t i = 0; i + names.size() <= raw.size() && !found; ++i) {
                bool all = true;
                for (size_t k = 0; k < names.size() && all; ++k) {
                    std::string uid, format;
                    all = raw[i + k].name + "|" + expectedState(raw[i + k], uid, format) == names[k];
                }
                found = all;
            }
            ordered = ordered && found;
        };
        for (const auto& t : p.tracks) {
            chainOrder(t.fx);
            for (const auto& e : t.events) chainOrder(e.fx);
        }
        for (const auto& b : p.buses) chainOrder(b.fx);
        chainOrder(p.audioOut);
        (void)from;
        check(ordered, name + ": every chain's effects are in the file's order");

        // saving: unchanged, and with every DirectX state saved again by "the plug-in" (the same bytes, as Reno's host
        // returns them) and every VST state turned into what Reno's host saves (Q: chunks; P: values), the effects'
        // entries are VEGAS's own bytes
        std::vector<std::string> warnings;
        const std::string same = writeVegasData(p, headerPath(bytes), &warnings);
        check(same == bytes, name + ": saved with no change, it is byte for byte the same");
        Project q = p;
        eachSlot(q, [&](FxSlot& s, const std::string&) {
            // as if the plug-in had saved its settings again in Reno (they no longer compare equal to what was read,
            // so the writer makes the entry anew from them instead of copying VEGAS's bytes)
            const auto* o = vegOriginAs<VegFxOrigin>(s.veg, VegOrigin::FxK);
            if (s.kind != "audio" || !o) return;
            auto changed = std::make_shared<VegFxOrigin>(*o);
            changed->asRead.state = std::string("Z:");
            s.veg = changed;
        });
        Project back;
        const std::string redone = writeVegasData(q, headerPath(bytes), &warnings);
        const VegasImportResult rb = importVegasData(redone, f, back);
        check(rb.ok, name + ": saved with every effect written anew, it opens again");
        std::multiset<std::string> again;
        std::vector<RawPlug> raw2 = rawPlugins(redone);
        size_t sameData = 0, vstRaw = 0;
        for (size_t i = 0; i < raw2.size(); ++i) {
            std::string uid, format;
            again.insert(slotKey(raw2[i].name, format, uid, (raw2[i].flags & 1) != 0, expectedState(raw2[i], uid, format)));
        }
        std::multiset<std::string> want2;
        for (const auto& x : raw) {
            std::string uid, format;
            want2.insert(slotKey(x.name, format, uid, (x.flags & 1) != 0, expectedState(x, uid, format)));
            (void)sameData; (void)vstRaw;
        }
        // (an effect written anew keeps its name, kind, id, switch and settings; the file may differ elsewhere)
        std::multiset<std::string> again2;
        for (const auto& x : raw2) {
            std::string uid, format;
            const std::string st = expectedState(x, uid, format);
            again2.insert(slotKey(x.name, format, uid, (x.flags & 1) != 0, st));
        }
        check(again2 == want2, name + ": effects written anew keep their names, ids, switches and settings exactly");
    }

    std::printf("%zu audio effects in %zu projects: %zu DirectX (%zu start with their own length), %zu VST2 (%zu with values, %zu with a chunk), %zu switched off\n",
                total, files.size(), dx, withPrefix, vst, params, chunked, off);
    for (const auto& kv : perPlugin) std::printf("   %4d  %s\n", kv.second, kv.first.c_str());
    if (root == testDataDir("RENO_SAMPLES")) {
        // the counts of the 37 sample projects (worked out from the bytes, also with a separate Python reading)
        check(files.size() == 37, "37 projects");
        check(total == 519, strfmt("519 audio effects (%zu)", total));
        check(dx == 403 && withPrefix == 403, strfmt("403 DirectX effects, every one's saved form starting with its length (%zu, %zu)", dx, withPrefix));
        check(vst == 116 && params == 73 && chunked == 43, strfmt("116 VST effects: 73 parameter programs, 43 chunk programs (%zu, %zu, %zu)", vst, params, chunked));
        check(off == 18, strfmt("18 switched off (%zu)", off));
    }

    // a VST plug-in's chunk state is written as VEGAS writes it: one program's chunk (FPCh), read back the same
    {
        const std::string chunk = "\x01\x02\x03\x04plug-in chunk";
        std::string orig;
        std::string prog;
        check(veg::vstProgram("Q:" + base64Encode(chunk.data(), chunk.size()), 0x4d617554u, veg::Bytes(), prog), "a Q: state makes a program");
        check(prog.size() == 60 + chunk.size() && prog.compare(8, 4, "FPCh") == 0 && prog.compare(60, std::string::npos, chunk) == 0 &&
                  be32(prog, 56) == chunk.size() && be32(prog, 16) == 0x4d617554u,
              "it is an FPCh program holding the chunk");
    }

    // Reno's VST2 host with a plug-in that keeps its settings as a chunk (tests/ui/strictvst.cpp): a program's chunk
    // from a .veg goes in, and the host saves its settings the same way, as one program's chunk
    if (!chunkVst.empty()) {
        std::string err;
        const auto descs = describeAudioPluginFile(chunkVst, &err);
        check(!descs.empty(), "the chunk VST loads (" + err + ")");
        if (!descs.empty()) {
            std::unique_ptr<AudioPlugin> pl = createAudioPlugin(descs[0], &err);
            check(pl != nullptr, "an instance is made");
            if (pl) {
                const std::string q = "Q:" + base64Encode("program chunk", 13);
                check(pl->loadState(q), "a program chunk (Q:) is taken");
                check(pl->saveState() == q, "and saved again as a program chunk: " + pl->saveState());
                check(pl->loadState("C:" + base64Encode("bank chunk", 10)) && pl->saveState() == "Q:" + base64Encode("bank chunk", 10),
                      "an older bank chunk (C:) is still taken");
            }
        }
    }

    if (!listOut.empty()) {
        std::string s;
        for (const auto& kv : distinct) s += kv.second + "\n";
        writeFile(listOut, s);
        std::printf("%zu distinct effects written to %s\n", distinct.size(), listOut.c_str());
    }
    std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else
// ------------------------------------------------------------------------------------------------ Windows

static bool saveWindowPicture(HWND w, const std::string& path)
{
    RECT rc;
    if (!GetClientRect(w, &rc)) return false;
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return false;
    HDC wdc = GetDC(w);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi;
    std::memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(w, mdc, PW_CLIENTONLY);
    BitBlt(mdc, 0, 0, W, H, wdc, 0, 0, SRCCOPY);
    std::string file;
    BITMAPFILEHEADER fh;
    std::memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + (DWORD)W * H * 4;
    file.append((const char*)&fh, sizeof(fh));
    file.append((const char*)&bi.bmiHeader, sizeof(bi.bmiHeader));
    file.append((const char*)bits, (size_t)W * H * 4);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(w, wdc);
    return writeFile(path, file);
}

static void pump(int ms)
{
    const DWORD end = GetTickCount() + (DWORD)ms;
    while (GetTickCount() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3 || std::strcmp(argv[1], "--plugins")) { std::printf("vegaudiofx_test --plugins list.txt [picture folder]\n"); return 2; }
    std::string list;
    if (!readFile(argv[2], list)) { std::printf("cannot read %s\n", argv[2]); return 2; }
    const std::string pictures = argc > 3 ? argv[3] : "";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // the VST plug-ins in the folders Reno looks in (VEGAS's own among them)
    const auto dirs = audioPluginSearchDirs();
    for (const auto& d : dirs) std::printf("plug-in folder: %s\n", d.c_str());
    const auto vegasDirs = vegasPluginDirs();
    check(!vegasDirs.empty(), "VEGAS's MAGIX Plugins folder is among the folders searched");
    std::map<std::string, AudioPluginDesc> vstById;
    for (const auto& f : findAudioPluginFiles(dirs)) {
        std::string err;
        for (const auto& d : describeAudioPluginFile(f, &err)) vstById[d.uid] = d;
        if (!err.empty()) std::printf("  %s: %s\n", f.c_str(), err.c_str());
    }
    std::printf("%zu VST plug-ins found\n", vstById.size());

    int shot = 0;
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        const std::string line = list.substr(at, end - at);
        at = end + 1;
        std::vector<std::string> f;
        size_t s = 0;
        for (;;) { size_t bar = line.find('|', s); f.push_back(line.substr(s, bar == std::string::npos ? std::string::npos : bar - s)); if (bar == std::string::npos) break; s = bar + 1; }
        if (f.size() < 5) continue;
        AudioPluginDesc desc;
        desc.format = f[0];
        desc.uid = f[1];
        desc.name = f[2];
        const std::string state = f[3];
        const std::string label = desc.name + " (" + desc.format + " " + (desc.format == "VST2" ? vstIdText(desc.uid) : desc.uid) + ")";
        if (desc.format == "VST2") {
            auto it = vstById.find(desc.uid);
            if (it == vstById.end()) {
                std::printf("not installed here: %s - %s\n", label.c_str(), audioPluginFileHint(desc).c_str());
                continue;
            }
            desc.path = it->second.path;
        }
        std::string err;
        std::unique_ptr<AudioPlugin> p = createAudioPlugin(desc, &err);
        check(p != nullptr, label + ": is made (" + err + ")");
        if (!p) continue;
        // as FxRuntime does: the settings first, then start it
        const bool loaded = p->loadState(state);
        check(loaded, label + ": takes its saved settings");
        check(p->activate(48000, 2048), label + ": starts");
        std::vector<float> l(4096), r(4096);
        for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = 0.25f * (float)std::sin(i * 0.05);
        p->process(l.data(), r.data(), 2048, 0, true);
        p->process(l.data() + 2048, r.data() + 2048, 2048, 2048, true);
        bool finite = true;
        for (size_t i = 0; i < l.size(); ++i) finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
        check(finite, label + ": runs");
        const std::string after = p->saveState();
        if (desc.format == "DX") {
            check(after == state, label + ": saves again exactly what it was given");
        } else if (state.compare(0, 2, "P:") == 0) {
            const std::string raw = base64Decode(state.substr(2));
            bool same = (int)(raw.size() / 4) <= p->paramCount() || p->paramCount() == 0;
            for (int k = 0; k < p->paramCount() && (size_t)k * 4 + 4 <= raw.size(); ++k) {
                float v;
                std::memcpy(&v, raw.data() + 4 * k, 4);
                // (eFX effects have a "Version" parameter that is the plug-in's own and does not take a value)
                if (std::fabs(p->paramGet(k) - v) > 1e-4f && p->paramName(k) != "Version") { same = false; std::printf("   %s: param %d (%s) %g, given %g\n", label.c_str(), k, p->paramName(k).c_str(), p->paramGet(k), v); }
            }
            check(same, label + ": its parameters are the given values");
        } else if (state.compare(0, 2, "Q:") == 0) {
            check(after == state, label + ": saves again the program chunk it was given");
        }
        std::printf("ok: %s%s\n", label.c_str(), loaded ? "" : " (settings NOT loaded)");
        // the plug-in's own window, opened after the settings went in (as Reno's effects window opens it)
        if (!pictures.empty() && desc.format == "DX") {
            check(p->hasEditor(), label + ": has a window of its own (property page)");
            HWND w = CreateWindowExW(0, L"STATIC", L"page", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, nullptr, nullptr);
            int ew = 0, eh = 0;
            const bool opened = p->openEditor(w, &ew, &eh);
            check(opened, label + ": its window opens");
            if (opened) {
                SetWindowPos(w, nullptr, 0, 0, ew, eh, SWP_NOZORDER | SWP_NOMOVE);
                pump(700);
                const std::string file = pathJoin(pictures, strfmt("page%02d.bmp", shot++));
                saveWindowPicture(w, file);
                std::printf("   window: %s\n", file.c_str());
                p->closeEditor();
            }
            DestroyWindow(w);
        }
        p->deactivate();
    }
    std::printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
#endif
