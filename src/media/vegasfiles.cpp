// VEGAS's peak files (.sfk) and audio proxies (.sfap0): see vegasfiles.h.
//
// ---- .sfk (all numbers little-endian) - checked sample for sample
// against the .wav files they were made from:
//   0  "SFPK"
//   4  u32 1                         version
//   8  u32 64                        size of this header (the peaks start here)
//   12 u32 local stamp               the media's time of last change as MS-DOS date (low 16 bits) and time (high 16
//                                    bits), local time (the offset from UTC in effect when the file was written)
//   16 u32 16                        bits of a peak (VEGAS's "Build 8-Bit Peak Files" preference would make it 8)
//   20 u32 channels                  the media's channels
//   24 u32 256                       samples a peak is nominally made from
//   28 i64 frames                    the media's length in samples, at its own rate (which is not stored)
//   36 u32 UTC stamp                 the same time, in UTC
//   40 24 zero bytes
//   64 the peaks: for each block, for each channel, i16 lowest then i16 highest sample: for a file of whole numbers
//      (integer PCM) its samples' top 16 bits, floor(sample * 32768); for anything decoded to floating point (float PCM,
//      AAC...) the sample rounded to the nearest. (Every number of the 16- and 24-bit .wav files' .sfk is matched; of a
//      float .wav's and two .mp4 files' 98-99%: the others are a block's edge sample VEGAS gives to the next block or
//      the one before, which no rule found yet explains.)
// Blocks: ceil(frames / 256) of them, spread evenly: block b is made from samples floor(b * frames / blocks) to
// floor((b + 1) * frames / blocks), both included (so neighbours share a sample) - which is why a block is a little
// shorter than 256 samples. The peak file of "x.wav" is "x.sfk"; of anything else "<name>.sfk" (vegas180k.dll:
// SfGetFileNameExtensionPtrW compared with ".wav"; ".sfk", or ".sfk<n>" for stream n > 0 of some media).
// VEGAS 18 also names peak files "<media>-0-<length in 100 ns>-1.sfk" (vegas180.exe formats "-%I64d" then "-%I64d-%d"
// onto the media's name): the sound of an .mp4 cut at the length of its frame (62.9 s of a 62.97 s AAC track), or
// of a nested project at its length in the parent. Their peaks are laid out the same way, over that length.
//
// ---- .sfap0 (a Sony Wave64 file; all chunk sizes include their 24-byte header, chunks start on 8-byte boundaries):
//   riff  66666972-912E-11CF-A5D6-28DB04C10000  size = the whole file; then the "wave" GUID
//   fmt   20746D66-ACF3-11D3-8CD1-00C04F8EDB8A  size 40: WAVEFORMAT + wBitsPerSample: 3 (IEEE float), 2 channels,
//                                               48000 (the project's rate), 384000 bytes/s, block 8, 32 bits
//   data  61746164-ACF3-11D3-8CD1-00C04F8EDB8A  the samples, interleaved L R
//   summary list 925F94BC-525A-11D2-86DC-00C04F8EDB8A  (Sonic Foundry's) u32 count 2, then entries of FOURCC, u32 byte
//         length, UTF-16 text with its 0: "TCOD" "0" and "TCDO" the end time in 100 ns (frames * 10^7 / rate,
//         rounded). The chunk's size is rounded up to a multiple of 8, the padding counted in it.
//   stamp 1D185A19-DD87-11D3-8102-00C04F60C0FA  size 44 (then 4 bytes of padding): u32 1, the FILETIME of the .veg's
//         last change when the proxy was made, u16 channels, u32 rate, u16 bits.
// vegas180.exe checks the stamp (the code that reads chunk 1D185A19): size >= 20, version 1, CompareFileTime(stamp,
// the project's) == 0, and channels / rate / bits equal to the format wanted - anything else and the proxy is
// built again. The 0-byte .sfap0 VEGAS leaves when a build is cancelled is not a proxy at all.
#include <mutex>
#include "vegasfiles.h"
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace reno {
namespace vegasfiles {

namespace {
#ifndef _WIN32
const int64_t kUnixEpochFileTime = 116444736000000000LL;      // 1970-01-01 as a FILETIME
#endif

const uint8_t kRiff[16] = {0x72, 0x69, 0x66, 0x66, 0x2e, 0x91, 0xcf, 0x11, 0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00};
const uint8_t kWave[16] = {0x77, 0x61, 0x76, 0x65, 0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};
const uint8_t kFmt[16] = {0x66, 0x6d, 0x74, 0x20, 0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};
const uint8_t kData[16] = {0x64, 0x61, 0x74, 0x61, 0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};
const uint8_t kSummary[16] = {0xbc, 0x94, 0x5f, 0x92, 0x5a, 0x52, 0xd2, 0x11, 0x86, 0xdc, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};
const uint8_t kStamp[16] = {0x19, 0x5a, 0x18, 0x1d, 0x87, 0xdd, 0xd3, 0x11, 0x81, 0x02, 0x00, 0xc0, 0x4f, 0x60, 0xc0, 0xfa};

void put16(std::string& s, uint16_t v) { s.append((const char*)&v, 2); }
void put32(std::string& s, uint32_t v) { s.append((const char*)&v, 4); }
void put64(std::string& s, uint64_t v) { s.append((const char*)&v, 8); }
uint16_t get16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

FILE* openFile(const std::string& path, const char* mode)
{
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void removeFile(const std::string& path)
{
#ifdef _WIN32
    DeleteFileW(widen(path).c_str());
#else
    std::remove(path.c_str());
#endif
}

// Renames 'from' over 'to' (replacing it).
bool replaceFile(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// Days since 1601-01-01 -> year, month, day (the proleptic Gregorian calendar).
void civil(int64_t days1601, int& y, int& m, int& d)
{
    int64_t z = days1601 - 134774 + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

// The offset of local time from UTC now, in 100 ns.
int64_t localBias()
{
#ifdef _WIN32
    FILETIME u, l;
    GetSystemTimeAsFileTime(&u);
    if (!FileTimeToLocalFileTime(&u, &l)) return 0;
    const int64_t a = ((int64_t)u.dwHighDateTime << 32) | u.dwLowDateTime, b = ((int64_t)l.dwHighDateTime << 32) | l.dwLowDateTime;
    return b - a;
#else
    const time_t now = std::time(nullptr);
    struct tm t;
    if (!localtime_r(&now, &t)) return 0;
    return (int64_t)t.tm_gmtoff * 10000000LL;
#endif
}
}  // namespace

// ====================================================================== times
int64_t fileTime(const std::string& path)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((int64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (int64_t)st.st_mtim.tv_sec * 10000000LL + st.st_mtim.tv_nsec / 100 + kUnixEpochFileTime;
#endif
}

uint32_t dosTime(int64_t ft, bool local)
{
    if (ft <= 0) return 0;
    if (local) ft += localBias();
    const int64_t twoSecs = (ft + 19999999) / 20000000;            // rounded up to an even second
    const int64_t secs = twoSecs * 2;
    const int64_t days = secs / 86400, rem = secs % 86400;
    int y, m, d;
    civil(days, y, m, d);
    if (y < 1980) return 0;
    const uint32_t date = (uint32_t)(((y - 1980) & 0x7f) << 9 | m << 5 | d);
    const uint32_t time = (uint32_t)((rem / 3600) << 11 | ((rem / 60) % 60) << 5 | (rem % 60) / 2);
    return date | time << 16;
}

// ====================================================================== .sfk
static std::mutex gPeakFolderMutex;
static std::string gPeakFolder;
void setPeakFolder(const std::string& folder)
{
    std::lock_guard<std::mutex> lk(gPeakFolderMutex);
    gPeakFolder = folder;
}
std::string peakFolder()
{
    std::lock_guard<std::mutex> lk(gPeakFolderMutex);
    return gPeakFolder;
}

static std::string besidePeakFilePath(const std::string& media)
{
    const std::string name = pathFileName(media);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && toLower(name.substr(dot)) == ".wav") return media.substr(0, media.size() - (name.size() - dot)) + ".sfk";
    return media + ".sfk";
}

std::string peakFilePath(const std::string& media)
{
    const std::string folder = peakFolder();
    if (folder.empty() || media.empty()) return besidePeakFilePath(media);
    uint32_t h = 2166136261u;
    for (unsigned char c : pathDir(media)) h = (h ^ c) * 16777619u;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x_", (unsigned)h);
    std::string name = pathFileName(media);
    return pathJoin(folder, std::string(buf) + name + ".sfk");
}

std::vector<std::string> peakFileCandidates(const std::string& media)
{
    std::vector<std::string> out;
    const std::string plain = peakFilePath(media);
    if (fileExists(plain)) out.push_back(plain);
    const std::string beside = besidePeakFilePath(media);
    if (beside != plain && fileExists(beside)) out.push_back(beside);
    // "<name>-<a>-<b>-<c>.sfk": the qualified ones, longest first
    const std::string dir = pathDir(media), name = pathFileName(media);
    std::vector<std::pair<int64_t, std::string>> q;
    for (const std::string& f : listDir(dir.empty() ? std::string(".") : dir)) {
        if (f.size() <= name.size() + 4 || !endsWithNoCase(f, ".sfk")) continue;
#ifdef _WIN32
        if (toLower(f.substr(0, name.size())) != toLower(name)) continue;
#else
        if (f.compare(0, name.size(), name) != 0) continue;
#endif
        const std::string rest = f.substr(name.size(), f.size() - name.size() - 4);       // "-0-629000000-1"
        long long a = 0, b = 0, c = 0;
        int used = 0;
        if (std::sscanf(rest.c_str(), "-%lld-%lld-%lld%n", &a, &b, &c, &used) != 3 || used != (int)rest.size()) continue;
        q.emplace_back(b, pathJoin(dir, f));
    }
    std::sort(q.begin(), q.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (const auto& e : q) out.push_back(e.second);
    return out;
}

int64_t peakBlocksFor(int64_t frames, int blockFrames)
{
    if (frames <= 0 || blockFrames <= 0) return 0;
    return (frames + blockFrames - 1) / blockFrames;
}

void PeakFile::blockRange(int64_t b, int64_t& first, int64_t& end) const
{
    const int64_t nb = std::max<int64_t>(1, peakBlocksFor(frames, blockFrames));
    first = (int64_t)((long double)b * frames / nb);
    end = std::min<int64_t>(frames, (int64_t)((long double)(b + 1) * frames / nb) + 1);
}

bool readPeakFile(const std::string& path, PeakFile& out, std::string* error)
{
    auto fail = [&](const char* e) { if (error) *error = e; return false; };
    std::string d;
    if (!readFile(path, d)) return fail("cannot be read");
    if (d.size() < 64 || d.compare(0, 4, "SFPK") != 0) return fail("not a peak file");
    const uint8_t* p = (const uint8_t*)d.data();
    const uint32_t version = get32(p + 4), header = get32(p + 8), bits = get32(p + 16), channels = get32(p + 20), spp = get32(p + 24);
    const int64_t frames = (int64_t)get64(p + 28);
    if (version != 1) return fail("an unknown version");
    if (header < 40 || header > d.size()) return fail("a bad header");
    if ((bits != 16 && bits != 8) || channels < 1 || channels > 64 || spp < 1 || spp > 65536 || frames < 0) return fail("a bad header");
    PeakFile pf;
    pf.channels = (int)channels;
    pf.blockFrames = (int)spp;
    pf.bits = (int)bits;
    pf.frames = frames;
    pf.localStamp = get32(p + 12);
    pf.utcStamp = get32(p + 36);
    const int64_t nb = peakBlocksFor(frames, (int)spp);
    const size_t each = bits / 8;
    const size_t need = (size_t)nb * channels * 2 * each;
    if (d.size() - header < need) return fail("cut short");
    pf.minmax.resize((size_t)nb * channels * 2);
    const uint8_t* v = p + header;
    if (bits == 16) std::memcpy(pf.minmax.data(), v, need);
    else
        for (size_t k = 0; k < pf.minmax.size(); ++k) pf.minmax[k] = (int16_t)((int)(int8_t)v[k] * 256);
    out = std::move(pf);
    return true;
}

bool writePeakFile(const std::string& path, const PeakFile& pf)
{
    if (pf.channels < 1 || pf.blocks() != peakBlocksFor(pf.frames, pf.blockFrames)) return false;
    std::string d("SFPK", 4);
    put32(d, 1);
    put32(d, 64);
    put32(d, pf.localStamp);
    put32(d, 16);
    put32(d, (uint32_t)pf.channels);
    put32(d, (uint32_t)pf.blockFrames);
    put64(d, (uint64_t)pf.frames);
    put32(d, pf.utcStamp);
    d.append(24, '\0');
    d.append((const char*)pf.minmax.data(), pf.minmax.size() * 2);
    const std::string part = path + ".part";
    if (!peakFolder().empty()) {
        std::error_code mk;
        std::filesystem::create_directories(std::filesystem::u8path(pathDir(path)), mk);
    }
    FILE* f = openFile(part, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(d.data(), 1, d.size(), f) == d.size();
    if (std::fclose(f) != 0 || !ok || !replaceFile(part, path)) {
        removeFile(part);
        return false;
    }
    return true;
}

bool peakFileCurrent(const PeakFile& pf, const std::string& media)
{
    const int64_t ft = fileTime(media);
    return ft > 0 && pf.utcStamp != 0 && pf.utcStamp == dosTime(ft, false);
}

bool peakRounding(const std::string& codec)
{
    const std::string c = toLower(codec);
    return !(c.compare(0, 5, "pcm_s") == 0 || c.compare(0, 5, "pcm_u") == 0);
}

PeakBuilder::PeakBuilder(int channels, int64_t frames, bool nearest) : nearest_(nearest)
{
    pf_.channels = std::max(1, channels);
    pf_.frames = std::max<int64_t>(0, frames);
    pf_.minmax.assign((size_t)peakBlocksFor(pf_.frames) * pf_.channels * 2, 0);
    lo_.assign((size_t)pf_.channels, INT_MAX);
    hi_.assign((size_t)pf_.channels, INT_MIN);
}

void PeakBuilder::add(const float* const* ch, int n)
{
    const int64_t nb = peakBlocksFor(pf_.frames);
    const int c = pf_.channels;
    const bool nearest = nearest_;
    auto quant = [nearest](float x) {
        const double v = nearest ? std::floor((double)x * 32768.0 + 0.5) : std::floor((double)x * 32768.0);
        return !(v == v) ? 0 : v < -32768 ? -32768 : v > 32767 ? 32767 : (int)v;
    };
    int64_t first = 0;
    if (done_ < nb && end_ == 0) pf_.blockRange(done_, first, end_);
    for (int k = 0; k < n && at_ < pf_.frames && done_ < nb; ++k, ++at_) {
        for (int j = 0; j < c; ++j) {
            const int q = quant(ch[j][k]);
            lo_[(size_t)j] = std::min(lo_[(size_t)j], q);
            hi_[(size_t)j] = std::max(hi_[(size_t)j], q);
        }
        if (at_ != end_ - 1) continue;
        // the block's last sample: it is done; the next one starts with this same sample (blocks share one)
        for (int j = 0; j < c; ++j) {
            pf_.minmax[(size_t)(done_ * c + j) * 2] = (int16_t)(lo_[(size_t)j] == INT_MAX ? 0 : lo_[(size_t)j]);
            pf_.minmax[(size_t)(done_ * c + j) * 2 + 1] = (int16_t)(hi_[(size_t)j] == INT_MIN ? 0 : hi_[(size_t)j]);
            lo_[(size_t)j] = INT_MAX;
            hi_[(size_t)j] = INT_MIN;
        }
        ++done_;
        if (done_ >= nb) {
            at_ = pf_.frames;
            break;
        }
        pf_.blockRange(done_, first, end_);
        if (first <= at_)
            for (int j = 0; j < c; ++j) lo_[(size_t)j] = hi_[(size_t)j] = quant(ch[j][k]);
    }
}

PeakFile PeakBuilder::finish(int64_t mediaFileTime)
{
    PeakFile pf = pf_;
    pf.localStamp = dosTime(mediaFileTime, true);
    pf.utcStamp = dosTime(mediaFileTime, false);
    return pf;
}

// ====================================================================== .sfap0
std::string audioProxyPath(const std::string& projectPath) { return projectPath + ".sfap0"; }

static bool parseProxy(const uint8_t* p, size_t n, int64_t fileSize, AudioProxyInfo& out, std::string* error)
{
    auto fail = [&](const char* e) { if (error) *error = e; return false; };
    if (n < 40 || std::memcmp(p, kRiff, 16) != 0 || std::memcmp(p + 24, kWave, 16) != 0) return fail("not a Wave64 file");
    AudioProxyInfo info;
    bool fmt = false, data = false;
    size_t at = 40;
    while (at + 24 <= n) {
        const uint8_t* c = p + at;
        const uint64_t size = get64(c + 16);
        if (size < 24) break;
        if (std::memcmp(c, kFmt, 16) == 0 && size >= 24 + 16 && at + 40 <= n) {
            info.format = get16(c + 24);
            info.channels = get16(c + 26);
            info.rate = (int)get32(c + 28);
            info.bits = get16(c + 38);
            fmt = true;
        } else if (std::memcmp(c, kData, 16) == 0) {
            info.dataOffset = (int64_t)at + 24;
            const int64_t bytes = std::min<int64_t>((int64_t)size - 24, fileSize - info.dataOffset);
            const int bpf = info.channels * info.bits / 8;
            info.frames = (fmt && bpf > 0 && bytes > 0) ? bytes / bpf : 0;
            data = true;
        } else if (std::memcmp(c, kStamp, 16) == 0 && size >= 24 + 20 && at + 44 <= n) {
            info.stamped = true;
            info.stampVersion = get32(c + 24);
            info.sourceTime = (int64_t)get64(c + 28);
            info.stampChannels = get16(c + 36);
            info.stampRate = (int)get32(c + 38);
            info.stampBits = get16(c + 42);
        }
        const uint64_t next = ((uint64_t)at + size + 7) & ~(uint64_t)7;
        if (next <= at || next > (uint64_t)fileSize) break;
        if (next > n) break;
        at = (size_t)next;
    }
    if (!fmt || !data) return fail("no format or no samples");
    if (info.channels < 1 || info.channels > 8 || info.rate < 1000 || info.rate > 768000) return fail("an unusable format");
    if (!((info.format == 3 && info.bits == 32) || (info.format == 1 && (info.bits == 16 || info.bits == 24)))) return fail("an unusable format");
    out = info;
    return true;
}

bool readAudioProxyInfo(const std::string& path, AudioProxyInfo& out, std::string* error)
{
    FILE* f = openFile(path, "rb");
    if (!f) {
        if (error) *error = "cannot be read";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
#ifdef _WIN32
    const int64_t size = _ftelli64(f);
#else
    const int64_t size = (int64_t)ftello(f);
#endif
    std::fseek(f, 0, SEEK_SET);
    // the header, and (the data chunk's size known) the chunks after the samples
    std::vector<uint8_t> head(4096);
    const size_t n = std::fread(head.data(), 1, head.size(), f);
    bool ok = parseProxy(head.data(), n, size, out, error);
    if (ok && out.dataOffset > 0) {
        const int64_t after = (out.dataOffset + out.frames * out.channels * (out.bits / 8) + 7) & ~(int64_t)7;
        if (after + 24 <= size && after - 40 > 0) {
            const int64_t tail = std::min<int64_t>(size - after, 4096);
            std::vector<uint8_t> t((size_t)tail);
#ifdef _WIN32
            _fseeki64(f, after, SEEK_SET);
#else
            fseeko(f, (off_t)after, SEEK_SET);
#endif
            const size_t got = std::fread(t.data(), 1, t.size(), f);
            for (size_t at = 0; at + 24 <= got;) {
                const uint8_t* c = t.data() + at;
                const uint64_t sz = get64(c + 16);
                if (sz < 24) break;
                if (std::memcmp(c, kStamp, 16) == 0 && sz >= 44 && at + 44 <= got) {
                    out.stamped = true;
                    out.stampVersion = get32(c + 24);
                    out.sourceTime = (int64_t)get64(c + 28);
                    out.stampChannels = get16(c + 36);
                    out.stampRate = (int)get32(c + 38);
                    out.stampBits = get16(c + 42);
                }
                // a size that is damaged - so large it wraps round - would bring this back to where it was, for ever.
                const uint64_t next = ((uint64_t)at + sz + 7) & ~(uint64_t)7;
                if (next <= at || next > got) break;
                at = (size_t)next;
            }
        }
    }
    std::fclose(f);
    return ok;
}

bool audioProxyCurrent(const AudioProxyInfo& info, int64_t projectFileTime)
{
    return info.stamped && info.stampVersion == 1 && projectFileTime != 0 && info.sourceTime == projectFileTime &&
           info.stampChannels == info.channels && info.stampRate == info.rate && info.stampBits == info.bits && info.frames > 0;
}

// ---- writing
AudioProxyWriter::~AudioProxyWriter() { abort(); }

bool AudioProxyWriter::open(const std::string& path, int rate, std::string* error)
{
    abort();
    path_ = path;
    part_ = path + ".part";
    rate_ = rate > 0 ? rate : 48000;
    frames_ = 0;
    FILE* f = openFile(part_, "wb");
    if (!f) {
        if (error) *error = "cannot write " + pathFileName(part_);
        return false;
    }
    file_ = f;
    std::string h((const char*)kRiff, 16);
    put64(h, 0);
    h.append((const char*)kWave, 16);
    h.append((const char*)kFmt, 16);
    put64(h, 40);
    put16(h, 3);                                    // IEEE float
    put16(h, 2);
    put32(h, (uint32_t)rate_);
    put32(h, (uint32_t)rate_ * 8);
    put16(h, 8);
    put16(h, 32);
    h.append((const char*)kData, 16);
    put64(h, 0);
    if (std::fwrite(h.data(), 1, h.size(), f) != h.size()) {
        if (error) *error = "cannot write " + pathFileName(part_);
        abort();
        return false;
    }
    return true;
}

bool AudioProxyWriter::write(const float* left, const float* right, int frames)
{
    if (!file_ || frames <= 0) return file_ != nullptr;
    buf_.resize((size_t)frames * 2);
    for (int k = 0; k < frames; ++k) {
        buf_[(size_t)k * 2] = left[k];
        buf_[(size_t)k * 2 + 1] = right[k];
    }
    if (std::fwrite(buf_.data(), sizeof(float), buf_.size(), (FILE*)file_) != buf_.size()) return false;
    frames_ += frames;
    return true;
}

bool AudioProxyWriter::finish(int64_t projectFileTime, std::string* error)
{
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        abort();
        return false;
    };
    if (!file_) return fail("nothing was written");
    FILE* f = (FILE*)file_;
    const uint64_t dataBytes = (uint64_t)frames_ * 8;
    std::string t;
    // the summary list: start "0", end in 100 ns, as UTF-16 text (Sonic Foundry's TCOD / TCDO)
    {
        const long long end = (long long)std::llround((double)frames_ * 1e7 / rate_);
        const std::string digits = std::to_string(end);
        std::string body;
        put32(body, 2);
        body.append("TCOD", 4);
        put32(body, 4);
        put16(body, '0');
        put16(body, 0);
        body.append("TCDO", 4);
        put32(body, (uint32_t)(digits.size() + 1) * 2);
        for (char ch : digits) put16(body, (uint16_t)ch);
        put16(body, 0);
        const uint64_t size = ((24 + body.size()) + 7) & ~(uint64_t)7;
        body.append((size_t)(size - 24 - body.size()), '\0');
        t.append((const char*)kSummary, 16);
        put64(t, size);
        t += body;
    }
    // the staleness mark
    t.append((const char*)kStamp, 16);
    put64(t, 44);
    put32(t, 1);
    put64(t, (uint64_t)projectFileTime);
    put16(t, 2);
    put32(t, (uint32_t)rate_);
    put16(t, 32);
    t.append(4, '\0');
    bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
    const uint64_t total = 40 + 40 + 24 + dataBytes + t.size();
    if (ok) {
        std::fflush(f);
        ok = std::fseek(f, 16, SEEK_SET) == 0 && std::fwrite(&total, 8, 1, f) == 1;
        const uint64_t dsize = 24 + dataBytes;
        ok = ok && std::fseek(f, 80 + 16, SEEK_SET) == 0 && std::fwrite(&dsize, 8, 1, f) == 1;
    }
    ok = (std::fclose(f) == 0) && ok;
    file_ = nullptr;
    if (!ok) return fail("cannot write " + pathFileName(part_));
    if (!replaceFile(part_, path_)) return fail("cannot replace " + pathFileName(path_) + " (is it in use?)");
    return true;
}

void AudioProxyWriter::abort()
{
    if (file_) {
        std::fclose((FILE*)file_);
        file_ = nullptr;
        removeFile(part_);
    }
}

// ---- reading
AudioProxyFile::~AudioProxyFile() { close(); }

bool AudioProxyFile::openMemory(int rate, std::shared_ptr<const std::vector<float>> left, std::shared_ptr<const std::vector<float>> right)
{
    close();
    if (!left || !right || left->size() != right->size() || rate < 1) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    memL_ = std::move(left);
    memR_ = std::move(right);
    info_ = AudioProxyInfo();
    info_.format = 3;
    info_.channels = 2;
    info_.rate = rate;
    info_.bits = 32;
    info_.frames = (int64_t)memL_->size();
    path_.clear();
    return true;
}

void AudioProxyFile::close()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!handle_) return;
#ifdef _WIN32
    CloseHandle((HANDLE)handle_);
#else
    ::close((int)(intptr_t)handle_ - 1);
#endif
    handle_ = nullptr;
}

bool AudioProxyFile::open(const std::string& path, std::string* error)
{
    close();
    AudioProxyInfo info;
    if (!readAudioProxyInfo(path, info, error)) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    memL_.reset();
    memR_.reset();
#ifdef _WIN32
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot be opened";
        return false;
    }
    handle_ = h;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error) *error = "cannot be opened";
        return false;
    }
    handle_ = (void*)(intptr_t)(fd + 1);
#endif
    path_ = path;
    info_ = info;
    return true;
}

bool AudioProxyFile::readAt(int64_t offset, void* to, size_t n)
{
    uint8_t* p = (uint8_t*)to;
    while (n > 0) {
#ifdef _WIN32
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)(offset & 0xffffffff);
        ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
        DWORD got = 0;
        const DWORD want = (DWORD)std::min<size_t>(n, 1 << 30);
        if (!ReadFile((HANDLE)handle_, p, want, &got, &ov) || got == 0) return false;
#else
        const ssize_t got = ::pread((int)(intptr_t)handle_ - 1, p, n, (off_t)offset);
        if (got <= 0) return false;
#endif
        p += got;
        n -= (size_t)got;
        offset += got;
    }
    return true;
}

bool AudioProxyFile::read(int64_t start, int frames, float* left, float* right)
{
    if (frames <= 0) return true;
    std::fill(left, left + frames, 0.f);
    std::fill(right, right + frames, 0.f);
    std::lock_guard<std::mutex> lk(mutex_);
    if (memL_) {
        const int64_t mf = std::max<int64_t>(0, start), mt = std::min<int64_t>(info_.frames, start + frames);
        if (mt > mf) {
            std::memcpy(left + (mf - start), memL_->data() + mf, (size_t)(mt - mf) * sizeof(float));
            std::memcpy(right + (mf - start), memR_->data() + mf, (size_t)(mt - mf) * sizeof(float));
        }
        return true;
    }
    if (!handle_) return false;
    const int64_t from = std::max<int64_t>(0, start), to = std::min<int64_t>(info_.frames, start + frames);
    if (to <= from) return true;
    const int ch = info_.channels, bps = info_.bits / 8;
    const size_t n = (size_t)(to - from);
    buf_.resize(n * ch * bps);
    if (!readAt(info_.dataOffset + from * ch * bps, buf_.data(), buf_.size())) return false;
    const size_t skip = (size_t)(from - start);
    for (size_t k = 0; k < n; ++k) {
        float v[2] = {0.f, 0.f};
        for (int c = 0; c < ch && c < 2; ++c) {
            const uint8_t* s = buf_.data() + (k * ch + c) * bps;
            if (info_.format == 3) std::memcpy(&v[c], s, 4);
            else if (bps == 2) v[c] = (float)(int16_t)get16(s) / 32768.f;
            else v[c] = (float)(((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24)) >> 8) / 8388608.f;
        }
        if (ch == 1) v[1] = v[0];
        left[skip + k] = v[0];
        right[skip + k] = v[1];
    }
    return true;
}

}  // namespace vegasfiles
}  // namespace reno
