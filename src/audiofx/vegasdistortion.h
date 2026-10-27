// VEGAS's Distortion DirectX effect (sfppack3_x64.dll, CLSID 39224540-6f92-11d0-aebc-00a0c9053912): its saved
// settings, presets and graph rules. Reno's built-in Distortion uses the same settings and saved form, so a project's
// VEGAS Distortion loads into it and it saves back as VEGAS Distortion.
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace reno {
namespace vegdist {

const char* const kClsid = "39224540-6f92-11d0-aebc-00a0c9053912";
const char* const kDxUid = "dshow:{39224540-6F92-11D0-AEBC-00A0C9053912}";
const int kInf = -840;        // the graph's "-Inf." (-84 dB); levels are in tenths of a dB
const int kMaxPoints = 32;

enum Mode { Individual = 0, Synchronize = 1, MirrorY = 2, MirrorX = 3, MirrorXY = 4 };

struct Point {
    int32_t x, y;             // level in, level out (tenths of a dB, -840..0)
};

// The plug-in's own layout (what it saves, after its length and a GUID).
struct Settings {
    int32_t lpOn = 0, lpFreq = 400;           // low-pass on the effect, Hz
    int32_t dry = -600, wet = 0;              // tenths of a dB; -600 is -Inf.
    int32_t slewOn = 0, slewRate = 100;       // 1..100
    int32_t mode = Individual;
    int32_t editPositive = 1;                 // the page shows the positive graph
    int32_t posCount = 2;
    Point pos[kMaxPoints] = {{kInf, kInf}, {0, 0}};
    int32_t negCount = 2;
    Point neg[kMaxPoints] = {{kInf, kInf}, {0, 0}};
    int32_t bypass = 0;
};
static_assert(sizeof(Settings) == 0x22c, "VEGAS Distortion's settings are 0x22c bytes");

// the GUID VEGAS Distortion writes before its settings
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0xe9, 0xc8, 0x20, 0xa7, 0x34, 0xd5, 0xf8, 0x4c, 0xa2, 0xdb, 0x83, 0xcf, 0xbd, 0xca, 0x8d, 0xc5};
    return g;
}

inline void clampPoints(Point* p, int32_t& n)
{
    n = std::max(2, std::min(kMaxPoints, (int)n));
    for (int k = 0; k < n; ++k) {
        p[k].x = std::max(kInf, std::min(0, (int)p[k].x));
        p[k].y = std::max(kInf, std::min(0, (int)p[k].y));
    }
    for (int k = n; k < kMaxPoints; ++k) p[k] = Point{0, 0};
}

inline void sanitize(Settings& s)
{
    s.lpOn = s.lpOn ? 1 : 0;
    s.lpFreq = std::max(100, std::min(10000, (int)s.lpFreq));
    s.dry = std::max(-600, std::min(0, (int)s.dry));
    s.wet = std::max(-600, std::min(240, (int)s.wet));
    s.slewOn = s.slewOn ? 1 : 0;
    s.slewRate = std::max(1, std::min(100, (int)s.slewRate));
    s.mode = std::max(0, std::min(4, (int)s.mode));
    s.editPositive = s.editPositive ? 1 : 0;
    clampPoints(s.pos, s.posCount);
    clampPoints(s.neg, s.negCount);
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x240), the GUID, the settings.
inline std::string encode(const Settings& s)
{
    std::string b(4 + 16 + sizeof(Settings), '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], persistGuid(), 16);
    std::memcpy(&b[20], &s, sizeof(Settings));
    return "S:" + base64Encode(b.data(), b.size());
}

inline bool decode(const std::string& st, Settings* out)
{
    if (st.size() < 3 || st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 20 + sizeof(Settings) || std::memcmp(b.data() + 4, persistGuid(), 16) != 0) return false;
    Settings s;
    std::memcpy(&s, b.data() + 20, sizeof(Settings));
    sanitize(s);
    *out = s;
    return true;
}

// The graph the page is not showing, made from the one it is, as the polarity mode says (VEGAS does this on every
// edit). Individual leaves it alone.
inline void applyMode(Settings& s)
{
    Point* from = s.editPositive ? s.pos : s.neg;
    const int32_t n = s.editPositive ? s.posCount : s.negCount;
    Point* to = s.editPositive ? s.neg : s.pos;
    int32_t& toN = s.editPositive ? s.negCount : s.posCount;
    auto clamp0 = [](int v) { return v < kInf ? kInf : (v > 0 ? 0 : v); };
    switch (s.mode) {
    case Synchronize:
        for (int k = 0; k < n; ++k) to[k] = from[k];
        break;
    case MirrorY:       // the gain turned upside down: out = 2 * in - out
        for (int k = 0; k < n; ++k) to[k] = Point{from[k].x, clamp0(2 * from[k].x - from[k].y)};
        break;
    case MirrorX: {     // reflected sideways across the diagonal: in = 2 * out - in, kept in order
        int hi = kInf;
        for (int k = 0; k < n; ++k) {
            if (k == n - 1) { to[k] = Point{0, 0}; continue; }
            const int x = 2 * from[k].y - from[k].x;
            hi = std::max(hi, x);
            to[k] = Point{clamp0(hi), from[k].y};
        }
        break;
    }
    case MirrorXY: {    // in and out swapped, kept in order
        int hx = kInf, hy = kInf;
        for (int k = 0; k < n; ++k) {
            if (k == n - 1) { to[k] = Point{0, 0}; continue; }
            hx = std::max(hx, (int)from[k].y);
            hy = std::max(hy, (int)from[k].x);
            to[k] = Point{hx, hy};
        }
        break;
    }
    default:
        return;
    }
    toN = n;
    for (int k = n; k < kMaxPoints; ++k) to[k] = Point{0, 0};
}

struct Preset {
    const char* name;
    Settings s;
};

inline Settings make(int dry, int wet, int slewOn, int slewRate, int mode, std::initializer_list<Point> pos, std::initializer_list<Point> neg)
{
    Settings s;
    s.dry = dry;
    s.wet = wet;
    s.slewOn = slewOn;
    s.slewRate = slewRate;
    s.mode = mode;
    s.posCount = (int32_t)pos.size();
    s.negCount = (int32_t)neg.size();
    int k = 0;
    for (const Point& p : pos) s.pos[k++] = p;
    k = 0;
    for (const Point& p : neg) s.neg[k++] = p;
    return s;
}

// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = [] {
        const std::initializer_list<Point> line = {{kInf, kInf}, {0, 0}};
        std::vector<Preset> v;
        v.push_back({"(Default)", make(-600, 0, 0, 100, Individual, line, line)});
        v.push_back({"2:1 Compression starting at -18 dB", make(-600, 0, 0, 100, Synchronize, {{kInf, kInf}, {-180, -180}, {0, -90}}, {{kInf, kInf}, {-180, -180}, {0, -90}})});
        v.push_back({"Soft noise gate", make(-600, 0, 0, 100, Synchronize, {{kInf, kInf}, {-500, kInf}, {-454, -600}, {-406, -460}, {-360, -360}, {0, 0}},
                                             {{kInf, kInf}, {-500, kInf}, {-454, -600}, {-406, -460}, {-360, -360}, {0, 0}})});
        v.push_back({"Fuzz 1", make(-600, 0, 0, 100, Synchronize, {{kInf, kInf}, {-440, -340}, {-240, -340}, {-240, -140}, {0, 0}},
                                    {{kInf, kInf}, {-440, -340}, {-240, -340}, {-240, -140}, {0, 0}})});
        v.push_back({"Fuzz 2", make(-600, 0, 0, 100, Individual, {{kInf, kInf}, {-440, -340}, {-240, -340}, {-240, -140}, {0, 0}},
                                    {{kInf, kInf}, {-340, -440}, {-240, -340}, {-140, -240}, {0, 0}})});
        v.push_back({"Wet slew", make(-600, 0, 1, 5, Individual, line, line)});
        v.push_back({"Slight chip fuzz", make(-600, 0, 1, 25, Individual, line, line)});
        v.push_back({"Transistor 1", make(-600, 0, 0, 100, Synchronize, {{kInf, kInf}, {-420, -660}, {-180, -180}, {0, -80}}, {{kInf, kInf}, {-420, -660}, {-180, -180}, {0, -80}})});
        v.push_back({"Transistor 2", make(-600, 0, 0, 100, Synchronize, {{kInf, kInf}, {-360, -720}, {-180, -180}, {0, -120}}, {{kInf, kInf}, {-360, -720}, {-180, -180}, {0, -120}})});
        v.push_back({"Lil' Warmer", make(-600, 0, 0, 100, MirrorX, {{kInf, kInf}, {-300, -240}, {0, 0}}, {{kInf, kInf}, {-180, -240}, {0, 0}})});
        v.push_back({"Tube 1", make(-50, -10, 0, 100, Synchronize, {{kInf, kInf}, {-710, -600}, {-540, -380}, {-300, -240}, {0, -120}},
                                    {{kInf, kInf}, {-710, -600}, {-540, -380}, {-300, -240}, {0, -120}})});
        v.push_back({"Boost positive signal", make(-600, -50, 0, 100, Synchronize, {{kInf, kInf}, {-290, -290}, {-290, -100}, {0, 0}},
                                                   {{kInf, kInf}, {-290, -290}, {-290, -100}, {0, 0}})});
        return v;
    }();
    return list;
}

}  // namespace vegdist
}  // namespace reno
