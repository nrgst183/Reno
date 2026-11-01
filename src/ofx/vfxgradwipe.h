// VEGAS Gradient Wipe (the older DirectX transition of vidpcore.dll), as it works from VEGAS's own code (read from the user's own VEGAS install and checked bit for
// bit against its functions; nothing of VEGAS is copied here): the gradient picture, the mask made from it and the mix of the two frames. No other dependencies
// than the standard library, so a test program can include it alone.
//
// What VEGAS does: each of its 28 gradients is a picture (a PNG kept as a resource of vidpcore.dll, which Reno reads at run time from an installed VEGAS and never
// carries). Per frame: (1) the picture's green byte g is cut into a mask byte by the progress p and the threshold blend b (the effect's "blend" value, 0 .. ~0.1):
//     p' = p (Direction Reverse: 1 - p); lo = (1 + b) p' - b; hi = (1 + b) p'
//     g <= int(lo x 255 + 0.5): 0;  g > int(hi x 255 + 0.5): 255;  between: (g - lo x 255) x 255 / ((hi - lo) x 255), cut to a byte  (Reverse: 255 less that, ends swapped)
// at the picture's own size, (2) the mask is stretched to the frame (a mean of the pixels it covers, rounded; the picture is cropped to the frame's shape first when the
// gradient keeps its aspect), (3) the frames are mixed a pixel at a time with the mask as weight of the FIRST frame in 14-bit fixed point:
//     w = (mask << 14) / 255;  out = (first x w + second x (16384 - w) + 8192) >> 14   (per byte, the alpha too)
// Horizontal / Vertical Mirror fold the mask about the middle (x > (w - 1) / 2 takes w - 1 - x), the Flips then turn it over (in that order).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace vgw {

// ------------------------------------------------------------------------------------------------------------ the 28 gradients
// VEGAS's gradient list in the order of its own index: the picture's resource number in vidpcore.dll (BINARY 2000 ..), whether the gradient keeps its aspect (a circle
// stays round; the linear ones and Box are stretched), and the threshold blend that choosing it sets.
struct Desc { int res; bool keepAspect; double blend; };
inline const Desc& desc(int vi)
{
    static const Desc t[28] = {
        {2000, true, 0.0},   {2027, true, 0.05},  {2001, true, 0.1},   {2002, true, 0.0},   {2003, true, 0.0},   {2004, true, 0.05},  {2005, true, 0.0},
        {2006, true, 0.1},   {2007, true, 0.03},  {2008, true, 0.0},   {2009, true, 0.0},   {2010, true, 0.0},   {2011, true, 0.0},   {2012, true, 0.05},
        {2013, true, 0.05},  {2014, true, 0.05},  {2015, true, 0.05},  {2016, false, 0.05}, {2017, false, 0.05}, {2018, false, 0.05}, {2019, false, 0.05},
        {2024, true, 0.0},   {2025, true, 0.0},   {2020, true, 0.05},  {2021, true, 0.05},  {2022, false, 0.0},  {2023, false, 0.0},  {2026, true, 0.03}};
    return t[vi < 0 ? 0 : (vi > 27 ? 27 : vi)];
}

// ------------------------------------------------------------------------------------------------------------ a small PNG reader (8 bits, not interlaced; only the green byte is kept)
struct Picture {
    int w = 0, h = 0;
    std::vector<uint8_t> g;     // w x h, row 0 at the top
};

namespace png {

struct Bits {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool bad = false;
    Bits(const uint8_t* d, size_t len) : p(d), n(len) {}
    int get(int need)
    {
        while (cnt < need) {
            if (pos >= n) { bad = true; return 0; }
            buf |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        const int v = (int)(buf & ((1u << need) - 1u));
        buf >>= need;
        cnt -= need;
        return v;
    }
};

struct Huff {
    uint16_t count[16];
    uint16_t symbol[320];
};

inline bool build(Huff& h, const uint8_t* len, int n)
{
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) ++h.count[len[i]];
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
    for (int i = 0; i < n; ++i)
        if (len[i]) h.symbol[offs[len[i]]++] = (uint16_t)i;
    return true;
}

inline int decodeSym(Bits& b, const Huff& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= b.get(1);
        if (b.bad) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// zlib stream -> bytes (false: damaged or longer than 'limit')
inline bool inflate(const uint8_t* d, size_t n, std::vector<uint8_t>& out, size_t limit)
{
    if (n < 2) return false;
    Bits b(d + 2, n - 2);
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int last = 0;
    while (!last) {
        last = b.get(1);
        const int type = b.get(2);
        if (b.bad) return false;
        if (type == 0) {
            b.buf = 0;
            b.cnt = 0;
            if (b.pos + 4 > b.n) return false;
            const unsigned len = b.p[b.pos] | ((unsigned)b.p[b.pos + 1] << 8);
            const unsigned nlen = b.p[b.pos + 2] | ((unsigned)b.p[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xffffu) != nlen || b.pos + len > b.n || out.size() + len > limit) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            if (type == 1) {
                uint8_t l[288];
                for (int i = 0; i < 144; ++i) l[i] = 8;
                for (int i = 144; i < 256; ++i) l[i] = 9;
                for (int i = 256; i < 280; ++i) l[i] = 7;
                for (int i = 280; i < 288; ++i) l[i] = 8;
                build(lit, l, 288);
                uint8_t dl[30];
                for (int i = 0; i < 30; ++i) dl[i] = 5;
                build(dist, dl, 30);
            } else {
                const int nlen = b.get(5) + 257, ndist = b.get(5) + 1, ncode = b.get(4) + 4;
                if (b.bad || nlen > 286 || ndist > 30) return false;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t lengths[320];
                std::memset(lengths, 0, sizeof(lengths));
                for (int i = 0; i < ncode; ++i) lengths[order[i]] = (uint8_t)b.get(3);
                if (b.bad) return false;
                Huff cl;
                if (!build(cl, lengths, 19)) return false;
                int idx = 0;
                uint8_t all[320];
                std::memset(all, 0, sizeof(all));
                while (idx < nlen + ndist) {
                    const int sym = decodeSym(b, cl);
                    if (sym < 0) return false;
                    if (sym < 16) all[idx++] = (uint8_t)sym;
                    else {
                        int rep, v = 0;
                        if (sym == 16) {
                            if (idx == 0) return false;
                            v = all[idx - 1];
                            rep = 3 + b.get(2);
                        } else if (sym == 17) rep = 3 + b.get(3);
                        else rep = 11 + b.get(7);
                        if (idx + rep > nlen + ndist) return false;
                        while (rep--) all[idx++] = (uint8_t)v;
                    }
                }
                if (all[256] == 0) return false;
                if (!build(lit, all, nlen) || !build(dist, all + nlen, ndist)) return false;
            }
            for (;;) {
                int sym = decodeSym(b, lit);
                if (sym < 0 || b.bad) return false;
                if (sym < 256) {
                    if (out.size() >= limit) return false;
                    out.push_back((uint8_t)sym);
                } else if (sym == 256) break;
                else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    const int len = lbase[sym] + b.get(lext[sym]);
                    const int ds = decodeSym(b, dist);
                    if (ds < 0 || ds >= 30) return false;
                    const size_t dd = (size_t)dbase[ds] + (size_t)b.get(dext[ds]);
                    if (b.bad || dd > out.size() || out.size() + (size_t)len > limit) return false;
                    const size_t from = out.size() - dd;
                    for (int i = 0; i < len; ++i) out.push_back(out[from + (size_t)i]);
                }
            }
        } else return false;
    }
    return true;
}

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// A PNG (8 bits a channel, grey / grey+alpha / RGB / RGBA, not interlaced) -> its green (grey) byte.
inline bool decode(const uint8_t* d, size_t n, Picture& pic)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n < 33 || std::memcmp(d, sig, 8) != 0) return false;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, inter = 1;
    std::vector<uint8_t> z;
    size_t i = 8;
    while (i + 12 <= n) {
        const uint32_t len = be32(d + i);
        const char* tag = (const char*)d + i + 4;
        if ((size_t)len > n - i - 12) return false;
        const uint8_t* body = d + i + 8;
        if (!std::memcmp(tag, "IHDR", 4) && len >= 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            ctype = body[9];
            inter = body[12];
        } else if (!std::memcmp(tag, "IDAT", 4)) z.insert(z.end(), body, body + len);
        else if (!std::memcmp(tag, "IEND", 4)) break;
        i += 12 + (size_t)len;
    }
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || depth != 8 || inter != 0) return false;
    int ch = 0, gi = 0;
    if (ctype == 0) { ch = 1; gi = 0; }
    else if (ctype == 2) { ch = 3; gi = 1; }
    else if (ctype == 4) { ch = 2; gi = 0; }
    else if (ctype == 6) { ch = 4; gi = 1; }
    else return false;
    const size_t stride = (size_t)w * (size_t)ch;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * h);
    if (!inflate(z.data(), z.size(), raw, (stride + 1) * h + 16) || raw.size() < (stride + 1) * h) return false;
    pic.w = (int)w;
    pic.h = (int)h;
    pic.g.assign((size_t)w * h, 0);
    std::vector<uint8_t> prev(stride, 0), cur(stride, 0);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* line = raw.data() + (size_t)y * (stride + 1);
        const int ft = line[0];
        const uint8_t* src = line + 1;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= (size_t)ch ? cur[x - (size_t)ch] : 0, up = prev[x], c = x >= (size_t)ch ? prev[x - (size_t)ch] : 0;
            int v = src[x];
            if (ft == 1) v += a;
            else if (ft == 2) v += up;
            else if (ft == 3) v += (a + up) >> 1;
            else if (ft == 4) {
                const int p = a + up - c, pa = std::abs(p - a), pb = std::abs(p - up), pc = std::abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c);
            } else if (ft != 0) return false;
            cur[x] = (uint8_t)v;
        }
        for (uint32_t x = 0; x < w; ++x) pic.g[(size_t)y * w + x] = cur[(size_t)x * (size_t)ch + (size_t)gi];
        prev.swap(cur);
    }
    return true;
}

}  // namespace png

// ------------------------------------------------------------------------------------------------------------ the resource in the DLL
// BINARY resource 'id' of a PE file (64 bit), read from the file without loading it: false when the file or the resource is not there.
inline bool readBinaryResource(const std::string& path, int id, std::vector<uint8_t>& out)
{
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    auto rd = [&](uint64_t off, void* dst, size_t n) {
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char*)dst, (std::streamsize)n);
        return (size_t)f.gcount() == n;
    };
    auto u32 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); };
    auto u16 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); };
    uint8_t dos[64];
    if (!rd(0, dos, 64) || dos[0] != 'M' || dos[1] != 'Z') return false;
    const uint32_t pe = u32(dos + 0x3c);
    uint8_t hdr[24 + 240];
    if (!rd(pe, hdr, sizeof(hdr)) || std::memcmp(hdr, "PE\0\0", 4) != 0) return false;
    const uint32_t nsec = u16(hdr + 6), osz = u16(hdr + 20);
    if (u16(hdr + 24) != 0x20b || osz < 128 + 16 || nsec == 0 || nsec > 96) return false;
    const uint32_t resRva = u32(hdr + 24 + 112 + 16);
    if (resRva == 0) return false;
    struct Sec { uint32_t va, vs, rs, ra; };
    std::vector<Sec> secs;
    for (uint32_t i = 0; i < nsec; ++i) {
        uint8_t s[40];
        if (!rd((uint64_t)pe + 24 + osz + 40u * i, s, 40)) return false;
        secs.push_back(Sec{u32(s + 12), u32(s + 8), u32(s + 16), u32(s + 20)});
    }
    auto toOff = [&](uint32_t rva, uint64_t& off) {
        for (const Sec& s : secs)
            if (rva >= s.va && rva < s.va + std::max(s.vs, s.rs)) { off = (uint64_t)s.ra + (rva - s.va); return true; }
        return false;
    };
    uint64_t base;
    if (!toOff(resRva, base)) return false;
    // one level of the resource tree: its entries (name or id, offset)
    struct Ent { uint32_t name, off; };
    auto dirOf = [&](uint32_t rel, std::vector<Ent>& e) {
        uint8_t h[16];
        if (!rd(base + rel, h, 16)) return false;
        const uint32_t cnt = u16(h + 12) + u16(h + 14);
        if (cnt > 4096) return false;
        e.clear();
        for (uint32_t i = 0; i < cnt; ++i) {
            uint8_t en[8];
            if (!rd(base + rel + 16 + 8u * i, en, 8)) return false;
            e.push_back(Ent{u32(en), u32(en + 4)});
        }
        return true;
    };
    std::vector<Ent> types;
    if (!dirOf(0, types)) return false;
    for (const Ent& t : types) {
        if (!(t.name & 0x80000000u) || !(t.off & 0x80000000u)) continue;
        uint8_t nl[2];
        if (!rd(base + (t.name & 0x7fffffffu), nl, 2)) return false;
        if (u16(nl) != 6) continue;
        uint8_t nm[12];
        if (!rd(base + (t.name & 0x7fffffffu) + 2, nm, 12)) return false;
        static const char* want = "BINARY";
        bool same = true;
        for (int k = 0; k < 6; ++k) same = same && nm[k * 2] == (uint8_t)want[k] && nm[k * 2 + 1] == 0;
        if (!same) continue;
        std::vector<Ent> ids;
        if (!dirOf(t.off & 0x7fffffffu, ids)) return false;
        for (const Ent& e : ids) {
            if ((e.name & 0x80000000u) || (int)e.name != id || !(e.off & 0x80000000u)) continue;
            std::vector<Ent> langs;
            if (!dirOf(e.off & 0x7fffffffu, langs) || langs.empty() || (langs[0].off & 0x80000000u)) return false;
            uint8_t de[16];
            if (!rd(base + langs[0].off, de, 16)) return false;
            const uint32_t rva = u32(de), size = u32(de + 4);
            uint64_t off;
            if (size == 0 || size > (64u << 20) || !toOff(rva, off)) return false;
            out.resize(size);
            return rd(off, out.data(), size);
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------------------ finding VEGAS's pictures
// The DLL to read: the file named by RENO_VIDPCORE_DLL (a test hook), else vidpcore.dll in the "Video Plug-Ins" folder of one of VEGAS's program folders ('vegasDirs').
inline std::string findDll(const std::vector<std::string>& vegasDirs)
{
    if (const char* e = std::getenv("RENO_VIDPCORE_DLL")) {
        if (*e) {
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::u8path(e), ec)) return e;
            return std::string();
        }
    }
    for (const std::string& d : vegasDirs) {
        for (const char* sub : {"Video Plug-Ins", "Video Plug-ins"}) {
            std::error_code ec;
            const std::filesystem::path p = std::filesystem::u8path(d) / sub / "vidpcore.dll";
            if (std::filesystem::exists(p, ec)) return p.u8string();
        }
    }
    return std::string();
}

// The picture of gradient 'vi' (VEGAS's index) from the DLL; null when it cannot be read. Decoded once and kept.
inline std::shared_ptr<const Picture> loadPicture(const std::string& dll, int vi)
{
    static std::mutex mu;
    static std::string cachedDll;
    static std::map<int, std::shared_ptr<const Picture>> cache;
    std::lock_guard<std::mutex> lock(mu);
    if (dll != cachedDll) {
        cache.clear();
        cachedDll = dll;
    }
    auto it = cache.find(vi);
    if (it != cache.end()) return it->second;
    std::shared_ptr<const Picture> result;
    std::vector<uint8_t> blob;
    if (!dll.empty() && readBinaryResource(dll, desc(vi).res, blob)) {
        auto p = std::make_shared<Picture>();
        if (png::decode(blob.data(), blob.size(), *p)) result = p;
    }
    cache[vi] = result;
    return result;
}

// ------------------------------------------------------------------------------------------------------------ the mask
// The mask byte for each picture byte at progress p with threshold blend b (VEGAS's 8-bit code, the same arithmetic in doubles).
inline void maskTable(double p, double b, bool reverse, uint8_t lut[256])
{
    const double pp = reverse ? 1.0 - p : p;
    const double hi = (1.0 + b) * pp;
    const double lo = (1.0 + b) * pp - b;
    const double range = hi - lo;
    double slope = 0.0;
    if (range > 9.999999974752427e-07) slope = 255.0 / (range * 255.0);
    const double lo255 = lo * 255.0, hi255 = hi * 255.0;
    const int r11 = (int)(lo255 + 0.5), ebx = (int)(hi255 + 0.5);
    double off = 0.0;
    int lowV = 0, highV = 255;
    if (reverse) {
        slope *= -1.0;
        lowV = 255;
        highV = 0;
        off = 255.0;
    }
    for (int g = 0; g < 256; ++g) {
        int v;
        if (g <= r11) v = lowV;
        else if (g > ebx) v = highV;
        else {
            const double x = ((double)g - lo255) * slope + off;
            v = x < 0.0 ? 0 : (int)(x > 255.0 ? 255.0 : x);
        }
        lut[g] = (uint8_t)v;
    }
}

// Auto Contrast Stretch's table (VEGAS's): the picture's darkest byte to 0, its brightest to 255, whole numbers between (the byte value x 255 / range, cut).
inline void contrastTable(int lo, int hi, uint8_t lut[256])
{
    const int range = hi - lo > 0 ? hi - lo : 0;
    for (int i = 0; i < 256; ++i) {
        if (i < lo) lut[i] = 0;
        else if (i < hi && range > 0) lut[i] = (uint8_t)(((i - lo) * 255) / range);
        else lut[i] = 255;
    }
}

struct Settings {
    double progress = 0, blend = 0;
    bool reverse = false, autoContrast = false, keepAspect = true;
};

// A source rectangle (picture pixels) stretched to W x H bytes: each frame pixel is the mean of the source it covers (whole numbers of pixels: the sum plus half the
// count, divided by the count; otherwise a coverage-weighted mean rounded to the nearest).
inline void stretchMean(const std::vector<uint8_t>& src, int sw, int sh, double x0, double y0, double vw, double vh, int W, int H, std::vector<uint8_t>& out)
{
    out.assign((size_t)W * H, 0);
    const double fx = vw / W, fy = vh / H;
    const bool whole = std::fabs(x0 - std::floor(x0 + 0.5)) < 1e-9 && std::fabs(y0 - std::floor(y0 + 0.5)) < 1e-9 && std::fabs(fx - std::floor(fx + 0.5)) < 1e-9 &&
                       std::fabs(fy - std::floor(fy + 0.5)) < 1e-9 && fx >= 1 && fy >= 1;
    if (whole) {
        const int n = (int)std::floor(fx + 0.5), m = (int)std::floor(fy + 0.5), ix = (int)std::floor(x0 + 0.5), iy = (int)std::floor(y0 + 0.5);
        const unsigned cnt = (unsigned)(n * m);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                unsigned s = 0;
                for (int j = 0; j < m; ++j) {
                    const int yy = std::min(sh - 1, std::max(0, iy + y * m + j));
                    const uint8_t* row = src.data() + (size_t)yy * sw;
                    for (int i = 0; i < n; ++i) s += row[std::min(sw - 1, std::max(0, ix + x * n + i))];
                }
                out[(size_t)y * W + x] = (uint8_t)((s + cnt / 2) / cnt);
            }
        return;
    }
    // weights of each frame column over the source columns / of each frame row over the source rows
    struct Tap { int first; std::vector<float> w; };
    auto taps = [](int n, double start, double step) {
        std::vector<Tap> t((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double a = start + i * step, b = a + step;
            const int j0 = (int)std::floor(a), j1 = (int)std::ceil(b);
            double tot = 0;
            std::vector<double> ws;
            for (int j = j0; j < j1; ++j) {
                const double w = std::min(b, (double)j + 1) - std::max(a, (double)j);
                ws.push_back(w > 0 ? w : 0);
                tot += ws.back();
            }
            t[(size_t)i].first = j0;
            t[(size_t)i].w.assign(ws.size(), 0.f);
            for (size_t k = 0; k < ws.size(); ++k) t[(size_t)i].w[k] = (float)(tot > 0 ? ws[k] / tot : 0);
        }
        return t;
    };
    const std::vector<Tap> tx = taps(W, x0, fx), ty = taps(H, y0, fy);
    int ry0 = sh, ry1 = -1;
    for (const Tap& t : ty) {
        ry0 = std::min(ry0, t.first);
        ry1 = std::max(ry1, t.first + (int)t.w.size() - 1);
    }
    ry0 = std::max(0, ry0);
    ry1 = std::min(sh - 1, ry1);
    if (ry1 < ry0) return;
    std::vector<float> mid((size_t)(ry1 - ry0 + 1) * W);
    for (int y = ry0; y <= ry1; ++y) {
        const uint8_t* row = src.data() + (size_t)y * sw;
        float* o = mid.data() + (size_t)(y - ry0) * W;
        for (int x = 0; x < W; ++x) {
            const Tap& t = tx[(size_t)x];
            double s = 0;
            for (size_t k = 0; k < t.w.size(); ++k) {
                const int j = std::min(sw - 1, std::max(0, t.first + (int)k));
                s += (double)t.w[k] * row[j];
            }
            o[x] = (float)s;
        }
    }
    for (int y = 0; y < H; ++y) {
        const Tap& t = ty[(size_t)y];
        for (int x = 0; x < W; ++x) {
            double s = 0;
            for (size_t k = 0; k < t.w.size(); ++k) {
                const int j = std::min(ry1, std::max(ry0, t.first + (int)k));
                s += (double)t.w[k] * mid[(size_t)(j - ry0) * W + x];
            }
            const int v = (int)(s + 0.5);
            out[(size_t)y * W + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

// The frame's mask (W x H bytes, row 0 at the top) for a gradient picture: contrast table, the visible part of the picture, the threshold, the stretch.
inline void frameMask(const Picture& pic, const Settings& s, int W, int H, std::vector<uint8_t>& out)
{
    const int pw = pic.w, ph = pic.h;
    // the part of the picture that the frame shows (picture pixels)
    double vx0 = 0, vy0 = 0, vw = pw, vh = ph;
    if (s.keepAspect && W > 0 && H > 0) {
        // the picture is cropped to the frame's shape about its middle; a square picture (the circles) is scaled so that the frame's corners lie on the picture's
        // corner circle, which leaves parts of the frame outside the picture (the gradient goes on there)
        double sc;
        if (pw == ph) sc = std::hypot((double)W, (double)H) / std::hypot((double)pw, (double)ph);
        else sc = std::max((double)W / pw, (double)H / ph);
        vw = W / sc;
        vh = H / sc;
        if (std::fabs(vw - pw) < 1e-6) vw = pw;
        if (std::fabs(vh - ph) < 1e-6) vh = ph;
        vx0 = (pw - vw) * 0.5;
        vy0 = (ph - vh) * 0.5;
    }
    std::vector<uint8_t> g = pic.g;
    if (s.autoContrast && !g.empty()) {
        int lo = 255, hi = 0;
        for (uint8_t v : g) {
            lo = std::min(lo, (int)v);
            hi = std::max(hi, (int)v);
        }
        uint8_t t[256];
        contrastTable(lo, hi, t);
        for (uint8_t& v : g) v = t[v];
    }
    const int cx0 = std::min(0, (int)std::floor(vx0 + 1e-9)), cy0 = std::min(0, (int)std::floor(vy0 + 1e-9));
    const int cx1 = std::max(pw, (int)std::ceil(vx0 + vw - 1e-9)), cy1 = std::max(ph, (int)std::ceil(vy0 + vh - 1e-9));
    const int cw = cx1 - cx0, ch = cy1 - cy0;
    std::vector<uint8_t> canvas;
    const uint8_t* cv = g.data();
    int canvasW = pw, canvasH = ph;
    double ox = 0, oy = 0;
    if (cw != pw || ch != ph) {
        // outside the picture the gradient is carried on along its slope at the edge (the circles' rings go on)
        canvas.assign((size_t)cw * ch, 0);
        const int k = 8;
        auto ext = [](double edge, double inner, int kk, int dist) {
            const double v = edge + (edge - inner) / kk * dist;
            return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : (int)(v + 0.5)));
        };
        for (int y = 0; y < ph; ++y) {
            const uint8_t* row = g.data() + (size_t)y * pw;
            uint8_t* o = canvas.data() + (size_t)(y - cy0) * cw;
            const int kx = std::min(k, pw - 1);
            for (int x = cx0; x < cx1; ++x) {
                if (x < 0) o[x - cx0] = kx > 0 ? ext(row[0], row[kx], kx, -x) : row[0];
                else if (x >= pw) o[x - cx0] = kx > 0 ? ext(row[pw - 1], row[pw - 1 - kx], kx, x - (pw - 1)) : row[pw - 1];
                else o[x - cx0] = row[x];
            }
        }
        const int ky = std::min(k, ph - 1);
        for (int y = cy0; y < cy1; ++y) {
            if (y >= 0 && y < ph) continue;
            uint8_t* o = canvas.data() + (size_t)(y - cy0) * cw;
            const uint8_t* e = canvas.data() + (size_t)((y < 0 ? 0 : ph - 1) - cy0) * cw;
            const uint8_t* in = canvas.data() + (size_t)((y < 0 ? ky : ph - 1 - ky) - cy0) * cw;
            for (int x = 0; x < cw; ++x) o[x] = ky > 0 ? ext(e[x], in[x], ky, y < 0 ? -y : y - (ph - 1)) : e[x];
        }
        cv = canvas.data();
        canvasW = cw;
        canvasH = ch;
        ox = -cx0;
        oy = -cy0;
    }
    uint8_t lut[256];
    maskTable(s.progress, s.blend, s.reverse, lut);
    std::vector<uint8_t> mask((size_t)canvasW * canvasH);
    for (size_t i = 0; i < mask.size(); ++i) mask[i] = lut[cv[i]];
    stretchMean(mask, canvasW, canvasH, vx0 + ox, vy0 + oy, vw, vh, W, H, out);
}

// The index of the mask pixel a frame pixel takes: Mirror folds about the middle, then Flip turns over.
inline int foldIndex(int i, int n, bool mirror, bool flip)
{
    if (mirror && i > (n - 1) / 2) i = n - 1 - i;
    if (flip) i = n - 1 - i;
    return i;
}

// The weight (of the FIRST frame) in 1/16384ths for a mask byte, and the mix of two bytes with it.
inline int weight14(int m) { return (m << 14) / 255; }
inline int mix8(int a, int b, int w) { return (a * w + b * (16384 - w) + 0x2000) >> 14; }

}  // namespace vgw
}  // namespace ofx
}  // namespace reno
