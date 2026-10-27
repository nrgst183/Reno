// Shared by Reno's versions of the sfppack3_x64.dll effects (vegassmooth.h, vegasgapper.h, vegasampmod.h, vegasvibrato.h,
// vegasflange.h; Distortion has its own, vegasdistortion.h): the saved form of a DirectX effect's settings in a VEGAS
// project, which is what the effect's IPersistStream::Save writes: u32 total length (itself included), a 16-byte GUID
// that is the effect's own, then its settings struct. In the project file it is kept as "S:" and base64.
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace reno {
namespace vegpack3 {

// the GUID as the bytes the effect writes (Windows GUID layout: three little-endian fields, then 8 bytes)
inline void guidBytes(const char* text, uint8_t out[16])
{
    // text: 8-4-4-4-12 hex digits with dashes
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    uint8_t raw[16];
    int n = 0;
    for (const char* p = text; *p && n < 16; ++p) {
        if (*p == '-') continue;
        raw[n++] = (uint8_t)(hex(p[0]) * 16 + hex(p[1]));
        ++p;
    }
    // d1 (4 bytes), d2, d3 (2 bytes each) are stored little-endian
    out[0] = raw[3]; out[1] = raw[2]; out[2] = raw[1]; out[3] = raw[0];
    out[4] = raw[5]; out[5] = raw[4];
    out[6] = raw[7]; out[7] = raw[6];
    for (int k = 8; k < 16; ++k) out[k] = raw[k];
}

// "S:" + base64 of [u32 length][GUID][settings]
inline std::string encode(const char* guid, const void* settings, size_t size)
{
    std::string b(4 + 16 + size, '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    uint8_t g[16];
    guidBytes(guid, g);
    std::memcpy(&b[4], g, 16);
    std::memcpy(&b[20], settings, size);
    return "S:" + base64Encode(b.data(), b.size());
}

// Reads the settings back (false: not this effect's saved form or too short).
inline bool decode(const char* guid, const std::string& st, void* settings, size_t size)
{
    if (st.size() < 3 || st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    uint8_t g[16];
    guidBytes(guid, g);
    if (b.size() < 20 + size || std::memcmp(b.data() + 4, g, 16) != 0) return false;
    std::memcpy(settings, b.data() + 20, size);
    return true;
}

// The effects: Reno's built-in id, the DirectX class id VEGAS saves them under and the name it gives them.
struct Entry {
    const char* uid;        // "reno.smooth"
    const char* clsid;      // lower case, with dashes
    const char* dxUid;      // as Reno names a DirectX plug-in: "dshow:{CLSID}"
    const char* dxName;     // the name in VEGAS's project
};
inline const Entry* entries(size_t* n)
{
    static const Entry list[] = {
        {"reno.flangewah", "f09f6980-7845-11d0-aebc-00a0c9053912", "dshow:{F09F6980-7845-11D0-AEBC-00A0C9053912}", "VEGAS Flange/Wah-Wah"},
        {"reno.gapsnip", "3f901a20-79be-11d0-aebc-00a0c9053912", "dshow:{3F901A20-79BE-11D0-AEBC-00A0C9053912}", "VEGAS Gapper/Snipper"},
        {"reno.vibrato", "d6802ba0-a056-11d0-aebc-00a0c9053912", "dshow:{D6802BA0-A056-11D0-AEBC-00A0C9053912}", "VEGAS Vibrato"},
        {"reno.smooth", "9e3e4540-8339-11d0-aebc-00a0c9053912", "dshow:{9E3E4540-8339-11D0-AEBC-00A0C9053912}", "VEGAS Smooth/Enhance"},
        {"reno.ampmod", "fdb0d300-6f82-11d0-aebc-00a0c9053912", "dshow:{FDB0D300-6F82-11D0-AEBC-00A0C9053912}", "VEGAS Amplitude Modulation"},
    };
    *n = sizeof list / sizeof list[0];
    return list;
}
// the built-in effect for a DirectX plug-in id ("dshow:{...}" in any case), or "" if it is not one of these
inline std::string builtinUidForDx(const std::string& dxUid)
{
    std::string l = dxUid;
    for (char& c : l) c = (char)std::tolower((unsigned char)c);
    size_t n = 0;
    const Entry* e = entries(&n);
    for (size_t k = 0; k < n; ++k)
        if (l.find(e[k].clsid) != std::string::npos) return e[k].uid;
    return std::string();
}
inline const Entry* entryForBuiltin(const std::string& uid)
{
    size_t n = 0;
    const Entry* e = entries(&n);
    for (size_t k = 0; k < n; ++k)
        if (uid == e[k].uid) return &e[k];
    return nullptr;
}

// a value in tenths of a dB as the gain the effects use: 10 ^ (v / 200)
inline double tenthsToGain(int32_t v) { return std::pow(10.0, (double)v / 200.0); }


// ---- tempo sync (Amplitude Modulation, Flange/Wah-wah): what the plug-ins are told about the project's tempo, and the length of
// one cycle that follows from a note length ----

// the beats per minute and the time signature, if known (the plug-ins ask the host for them)
struct Tempo {
    bool haveBpm = true;
    double bpm = 120.0;
    bool haveMeter = true;
    int numerator = 4;
    int denominator = 4;
};
// Samples in one cycle: a note length 'period' (0 .. 17: 64th note triplet to dotted half note, in beats of a quarter note; 18: measures)
// times the multiplier (in thousandths) at 'rate' samples per second, in the plug-ins' order of operations.
inline double syncCycle(const Tempo& t, int period, int multiplier, unsigned rate)
{
    static const double kNote[18] = {0.041666666666666664, 0.0625, 0.09375, 0.08333333333333333, 0.125, 0.1875, 0.16666666666666666, 0.25, 0.375,
                                     0.3333333333333333, 0.5, 0.75, 0.6666666666666666, 1.0, 1.5, 1.3333333333333333, 2.0, 3.0};
    const double mult = (double)multiplier / 1000.0;
    double beat = 2.0;
    if (t.haveBpm) beat = 60.0 / t.bpm;
    double len;
    if (period == 18) {
        double measure = 4.0;
        if (t.haveMeter) measure = 4.0 / (double)t.denominator * (double)t.numerator;
        len = measure * beat;
    } else {
        len = beat * kNote[std::max(0, std::min(17, period))];
    }
    len = len * mult;
    return len * (double)rate;
}

// ---- what the effects with a graph (Vibrato, Amplitude Modulation) do with it: the same code in both plug-ins ----

// the processor's conversion of a double to an int: out of range (and not a number) gives the smallest int
inline int cvtt(double x)
{
    if (!(x > -2147483649.0 && x < 2147483648.0)) return (int)0x80000000u;
    return (int)x;
}
inline int sub32(int a, int b) { return (int)((uint32_t)a - (uint32_t)b); }

// straight line from the table's entry a to b, with the entries between them filled in
inline void graphLine(double* t, int a, int b)
{
    int lo = a, hi = b;
    if (a > b) {
        lo = b;
        hi = a;
    }
    const double span = (double)(hi - lo);
    if (span == 0.0) return;
    const double slope = (t[hi] - t[lo]) / span;
    for (int k = lo + 1; k < hi; ++k) t[k] = (double)(k - lo) * slope + t[lo];
}

// The graph's points (count pairs x, y of whole numbers: x 0 to 1000 along one cycle, y from yLo to yHi) into the table
// of 1001 values, a straight line between neighbours. With 'blend' the plug-in first puts a point at 50 and one at 950 on
// the lines out of and into the end points (when those are further away), then gives both ends the mean of their heights.
inline void fillGraph(double* tbl, const int32_t* pts, int count, bool blend, int yLo, int yHi)
{
    int p[2 * (32 + 2)];
    std::memset(p, 0, sizeof p);
    int n = std::max(2, std::min(32, count));
    std::memcpy(p, pts, (size_t)n * 8);
    auto clampY = [&](int y) { return y < yLo ? yLo : std::min(y, yHi); };
    if (blend) {
        const int y0 = p[1];
        const int mid = (p[2 * (n - 1) + 1] + y0) / 2;     // towards zero
        if (p[2] > 50) {
            const double slope = (double)(p[3] - y0) / (double)p[2];
            for (int k = n - 1; k >= 1; --k) {
                p[2 * (k + 1)] = p[2 * k];
                p[2 * (k + 1) + 1] = p[2 * k + 1];
            }
            ++n;
            p[2] = 50;
            p[3] = clampY(sub32(y0, cvtt(slope * -50.0)));
        }
        if (p[2 * (n - 2)] < 950) {
            const int xa = p[2 * (n - 2)];
            const double slope = (double)(p[2 * (n - 1) + 1] - p[2 * (n - 2) + 1]) / (double)(p[2 * (n - 1)] - xa);
            p[2 * n] = p[2 * (n - 1)];
            p[2 * n + 1] = p[2 * (n - 1) + 1];
            ++n;
            p[2 * (n - 2)] = 950;
            p[2 * (n - 2) + 1] = clampY(sub32(p[2 * (n - 1) + 1], cvtt(slope * 50.0)));
        }
        const int e = clampY(mid);
        p[2 * (n - 1) + 1] = e;
        p[1] = e;
    }
    for (int k = 0; k + 1 < n; ++k) {
        tbl[p[2 * k]] = (double)p[2 * k + 1];
        tbl[p[2 * (k + 1)]] = (double)p[2 * (k + 1) + 1];
        graphLine(tbl, p[2 * (k + 1)], p[2 * k]);
    }
}

}  // namespace vegpack3
}  // namespace reno
