// VEGAS's Graphic Dynamics DirectX effect (sfppack2_x64.dll, CLSID f3b8e880-b4e0-11d0-aebc-00a0c9053912): its saved
// settings, presets and its processing, worked out from the DLL's own code. Reno's built-in Graphic Dynamics uses the
// same settings and saved form, so a project's VEGAS Graphic Dynamics loads into it and it saves back as VEGAS Graphic
// Dynamics.
//
// The sound is a block compressor / gate: 20 ms of the sound are collected, the loudest sample of the block (of each
// channel, or of all of them together with "Sync channel gains") is looked up on the graph (a table of the gain at each
// tenth of a dB from 0 to -84 dB made from the graph's points), the gain of the channel moves from where it was towards
// that value (a one pole step per block, the faster attack or the slower release) and the block is output with the gain
// ramped over the attack or release time. The sound comes out one block late (block()).
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace veggd {

const char* const kClsid = "f3b8e880-b4e0-11d0-aebc-00a0c9053912";
const char* const kDxUid = "dshow:{F3B8E880-B4E0-11D0-AEBC-00A0C9053912}";

const int kMaxPoints = 16;

// The plug-in's own layout (what it saves, after its length and a GUID): 0xa4 bytes. The graph is a list of points
// (input level, output level) in tenths of a dB, sorted by the input level, -840 (= -Inf.) .. 0.
struct Settings {
    int32_t outGain = 0;                // tenths of a dB, -250..250
    int32_t attack = 10;                // tenths of a ms, 0..5000
    int32_t release = 5000;             // tenths of a ms, 0..50000
    int32_t autoGain = 0;               // "Auto gain compensate"
    int32_t sync = 1;                   // "Sync channel gains"
    int32_t pt[kMaxPoints][2] = {{-840, -840}, {-240, -240}, {0, 0}};
    int32_t count = 3;                  // the points in use, 2..16
    int32_t threshold = -240;           // tenths of a dB: found again from the points when it is saved
    int32_t ratio = 10;                 // tenths: 10 (1.0:1) .. 500 (Inf.:1), found again from the points when it is saved
    int32_t bypass = 0;

    bool operator==(const Settings& o) const { return std::memcmp(this, &o, sizeof(Settings)) == 0; }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
static_assert(sizeof(Settings) == 0xa4, "VEGAS Graphic Dynamics's settings are 0xa4 bytes");

// The GUID VEGAS Graphic Dynamics writes before its settings (length 0xb8); it also reads f3b8e883-b4e0-11d0-aebc-
// 00a0c9053912 with length 0xb4 (an older form: the settings without the last field).
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0xa2, 0x33, 0x71, 0x5f, 0x34, 0x9a, 0x2e, 0x4e, 0xa7, 0xbd, 0xb6, 0x2a, 0x4a, 0x9d, 0xd5, 0xf5};
    return g;
}
inline const uint8_t* persistGuidOld()
{
    static const uint8_t g[16] = {0x83, 0xe8, 0xb8, 0xf3, 0xe0, 0xb4, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
    return g;
}

// The plug-in sorts the points by the input level (a bubble sort that swaps only a larger one before a smaller one)
inline void sortPoints(Settings& s)
{
    for (int pass = s.count - 1; pass > 0; --pass)
        for (int k = 0; k < pass; ++k)
            if (s.pt[k][0] > s.pt[k + 1][0]) {
                std::swap(s.pt[k][0], s.pt[k + 1][0]);
                std::swap(s.pt[k][1], s.pt[k + 1][1]);
            }
}

// The threshold the plug-in shows and saves: found from the (sorted) points as the highest one (not the first or the
// last) that is within 1 dB of the line where the level out is the level in; -90 dB if none (SfGdyn 0x18003cc90).
// 'index' says which point it is.
inline int thresholdOf(const Settings& s, int* index = nullptr)
{
    int best = -900, at = s.count - 2;
    for (int k = s.count - 2; k > 0; --k) {
        const int d = std::abs(s.pt[k][0] - s.pt[k][1]);
        if (d < 10 && s.pt[k][0] > best) {
            best = s.pt[k][0];
            at = k;
        }
    }
    if (index) *index = at;
    return s.pt[at < 0 ? 0 : at][0];
}

// The ratio (in tenths, 10..500) from the threshold's point to the last point (SfGdyn 0x18003d6a0)
inline int ratioOf(const Settings& s, int thresholdIndex)
{
    const int dy = s.pt[s.count - 1][1] - s.pt[thresholdIndex][1];
    if (dy <= 0) return 500;
    const int dx = s.pt[s.count - 1][0] - s.pt[thresholdIndex][0];
    const double mid = (double)dx * 10.0;
    int half = (int)((unsigned)std::abs(dy) >> 1);
    if (0.0 > mid) half = -half;
    const double q = ((double)half + mid) / (double)dy;
    int r = (q > -2147483649.0 && q < 2147483648.0) ? (int)q : (int)0x80000000u;
    if (r < 1) return 1;
    return r > 500 ? 500 : r;
}

// What the plug-in's setters keep: ranges, and the points inside the graph (the plug-in itself does not check them)
inline void sanitize(Settings& s)
{
    s.outGain = std::max(-250, std::min(250, (int)s.outGain));
    s.attack = std::max(0, std::min(5000, (int)s.attack));
    s.release = std::max(0, std::min(50000, (int)s.release));
    s.autoGain = s.autoGain ? 1 : 0;
    s.sync = s.sync ? 1 : 0;
    s.bypass = s.bypass ? 1 : 0;
    if (s.count < 2 || s.count > kMaxPoints) {
        const Settings d;
        std::memcpy(s.pt, d.pt, sizeof s.pt);
        s.count = d.count;
    }
    for (int k = 0; k < kMaxPoints; ++k) {
        if (k >= s.count) {
            s.pt[k][0] = 0;
            s.pt[k][1] = 0;
            continue;
        }
        s.pt[k][0] = std::max(-840, std::min(0, (int)s.pt[k][0]));
        s.pt[k][1] = std::max(-840, std::min(0, (int)s.pt[k][1]));
    }
    sortPoints(s);
    s.threshold = std::max(-800, std::min(-1, (int)s.threshold));
    s.ratio = std::max(10, std::min(500, (int)s.ratio));
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0xb8), the GUID, the settings. The threshold and the
// ratio in it come from the points, as the plug-in works them out when it saves.
inline std::string encode(const Settings& in)
{
    Settings s = in;
    sanitize(s);
    int at = 0;
    s.threshold = thresholdOf(s, &at);
    s.ratio = ratioOf(s, at);
    std::string b(4 + 16 + sizeof(Settings), '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], persistGuid(), 16);
    std::memcpy(&b[20], &s, sizeof(Settings));
    return "S:" + base64Encode(b.data(), b.size());
}

inline bool decode(const std::string& st, Settings* out)
{
    if (st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 20) return false;
    uint32_t len;
    std::memcpy(&len, b.data(), 4);
    Settings s;
    if (std::memcmp(b.data() + 4, persistGuid(), 16) == 0 && len == 20 + sizeof(Settings) && b.size() >= len) {
        std::memcpy(&s, b.data() + 20, sizeof(Settings));
    } else if (std::memcmp(b.data() + 4, persistGuidOld(), 16) == 0 && len == 20 + sizeof(Settings) - 4 && b.size() >= len) {
        std::memcpy(static_cast<void*>(&s), b.data() + 20, sizeof(Settings) - 4);
        s.bypass = 0;
    } else {
        return false;
    }
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    std::string name;
    Settings s;
};

// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = [] {
        auto mk = [](int outGain, int attack, int release, int autoGain, int sync, std::vector<std::pair<int, int>> pts, int count, int thr, int ratio,
                     int bypass) {
            Settings s;
            s.outGain = outGain;
            s.attack = attack;
            s.release = release;
            s.autoGain = autoGain;
            s.sync = sync;
            for (int k = 0; k < kMaxPoints; ++k) {
                s.pt[k][0] = k < (int)pts.size() ? pts[(size_t)k].first : 0;
                s.pt[k][1] = k < (int)pts.size() ? pts[(size_t)k].second : 0;
            }
            s.count = count;
            s.threshold = thr;
            s.ratio = ratio;
            s.bypass = bypass;
            return s;
        };
        std::vector<Preset> v;
        v.push_back({"(Default)", mk(0, 10, 5000, 0, 1, {{-840, -840}, {-240, -240}, {0, 0}}, 3, -210, 10, 0)});
        v.push_back({"Default all parameters", mk(0, 10, 5000, 0, 1, {{-840, -840}, {-240, -240}, {0, 0}}, 3, -210, 10, 0)});
        v.push_back({"Compressor/Gate", mk(0, 10, 5000, 1, 1, {{-840, -840}, {-614, -840}, {-150, -150}, {0, -119}}, 4, -150, 48, 0)});
        v.push_back({"Mute levels below -35 dB (hard noise gate)", mk(0, 10, 5000, 0, 1, {{-840, -840}, {-351, -840}, {-350, -350}, {0, 0}}, 4, -350, 10, 0)});
        v.push_back({"Limit levels to -6 dB (hard limiter)", mk(0, 10, 5000, 0, 1, {{-840, -840}, {-60, -60}, {0, -60}}, 3, -60, 500, 0)});
        v.push_back({"Soft limiter", mk(0, 10, 5000, 0, 1, {{-840, -840}, {-122, -124}, {-57, -78}, {0, -68}}, 4, -122, 22, 0)});
        v.push_back({"Maximize volume (-12 dB limiter with gain compensation)", mk(0, 50, 1000, 1, 0, {{-840, -840}, {-120, -120}, {0, -120}}, 3, -120, 500, 0)});
        v.push_back({"2:1 compression starting at -18 dB", mk(0, 10, 5000, 1, 1, {{-840, -840}, {-180, -180}, {0, -90}}, 3, -180, 20, 0)});
        v.push_back({"3:1 compression starting at -15 dB", mk(0, 10, 5000, 1, 1, {{-840, -840}, {-150, -150}, {0, -100}}, 3, -150, 30, 0)});
        v.push_back({"Soft knee compressor/gate (-24 dB threshold)",
                     mk(0, 10, 5000, 1, 1, {{-840, -840}, {-456, -840}, {-419, -562}, {-334, -361}, {-240, -238}, {-110, -150}, {0, -119}}, 7, -240, 20, 0)});
        v.push_back({"Soft noise gate below -36 dB", mk(0, 10, 10000, 0, 1, {{-840, -840}, {-557, -840}, {-515, -672}, {-443, -490}, {-338, -341}, {0, 0}}, 6, -338, 10, 0)});
        return v;
    }();
    return list;
}

// ------------------------------------------------------------------------------------------------- what the page does
// The edits the page's threshold and ratio sliders and its Reset Graph button make to the graph, as the plug-in's dialog
// does them (SfGdyn 0x18003d730, 0x18003d8a0 and the handlers in its dialog procedure near 0x18003ee0d..0x18003f260).
// The points are worked on in a buffer with room for one more than the most there can be.
struct Edit {
    int32_t pt[kMaxPoints + 2][2];
    int count;
    explicit Edit(const Settings& s)
    {
        std::memset(pt, 0, sizeof pt);
        std::memcpy(pt, s.pt, sizeof s.pt);
        count = s.count;
    }
    void store(Settings& s) const
    {
        std::memcpy(s.pt, pt, sizeof s.pt);
        s.count = count;
    }
};

inline int toInt32(double x)
{
    if (!(x > -2147483649.0 && x < 2147483648.0)) return (int)0x80000000u;
    return (int)x;
}

// the threshold point of an array of points (like thresholdOf) and the ratio from it (like ratioOf)
inline int thresholdAt(const int32_t (*pt)[2], int count, int* index)
{
    int best = -900, at = count - 2;
    for (int k = count - 2; k > 0; --k) {
        const int d = std::abs(pt[k][0] - pt[k][1]);
        if (d < 10 && pt[k][0] > best) {
            best = pt[k][0];
            at = k;
        }
    }
    *index = at;
    return pt[at < 0 ? 0 : at][0];
}

// Points that came too near the ends (an input level of -83.7 dB or lower, or -0.3 dB or higher) are taken out of the
// graph; the points after them move up (and a level below -80 dB that is not -Inf. is kept at -80 dB).
inline void dropCollapsed(Edit& e)
{
    if (e.count <= 3) return;
    int k = 1;
    while (k < e.count - 1) {
        const int x = e.pt[k][0];
        if (x <= -837 || x >= -3) {
            for (int j = k; j < e.count; ++j) {
                e.pt[j][0] = e.pt[j + 1][0];
                e.pt[j][1] = e.pt[j + 1][1];
                if (e.pt[j][1] != -840 && e.pt[j][1] < -800) e.pt[j][1] = -800;
            }
            --e.count;
        }
        ++k;
    }
}

// the points above the threshold point move up or down by 'delta' (at the last point in full, at the others in
// proportion to their distance from the threshold point), kept between the threshold point and the last point
inline void moveLast(Edit& e, int idx, int delta)
{
    const int n = e.count, last = n - 1;
    const int base = e.pt[idx][0];
    if ((unsigned)last > (unsigned)idx) {
        const unsigned half = (unsigned)std::abs(base) >> 1;
        for (int j = last; j > idx; --j) {
            const double t = (double)(base - e.pt[j][0]) * (double)delta;
            int add = (int)half;
            if (0.0 > t) add = -add;
            const double q = (t + (double)add) / (double)base;
            e.pt[j][1] += toInt32(q);
            const int lo = e.pt[idx][1];
            const int v = e.pt[j][1];
            if (v < lo) e.pt[j][1] = lo;
            else if (v > e.pt[n - 1][1]) e.pt[j][1] = e.pt[n - 1][1];
        }
    }
    dropCollapsed(e);
}

// the points between the first and the last move along the diagonal by 'delta'; the last point's level follows
inline void moveKnee(Edit& e, int delta)
{
    const int n = e.count;
    for (int k = 1; k < n - 1; ++k) {
        int y = e.pt[k][1];
        if (y != -840) {
            y += delta;
            if (y < -840) y = -840;
            else if (y > 0) y = 0;
            e.pt[k][1] = y;
        }
        const int x = e.pt[k][0] + delta;
        const int lo = e.pt[k - 1][0] + 3;
        if (x < lo) {
            e.pt[k][0] = lo;
        } else {
            const int hi = e.pt[k + 1][0] - 3;
            e.pt[k][0] = x > hi ? hi : x;
        }
    }
    const int L = n - 1;
    const int yl = e.pt[L][1];
    if (yl != 0) {
        const int moved = yl + delta;
        const int dx = delta - e.pt[L - 1][0] + e.pt[L][0];
        const int dy = yl - e.pt[L - 1][1] + delta;
        e.pt[L][1] = moved;
        if (dx != 0) {
            const double t = (double)dy * (double)delta;
            int add = (int)((unsigned)std::abs(dx) >> 1);
            if (0.0 > t) add = -add;
            const double q = ((double)add + t) / (double)dx;
            e.pt[L][1] = moved - toInt32(q);
        }
        int v = e.pt[L][1];
        if (v < -840) v = -840;
        else if (v > 0) v = 0;
        e.pt[L][1] = v;
    }
    dropCollapsed(e);
}

// the level the last point has to be at for a ratio (tenths) above the threshold point
inline int lastLevelFor(const Edit& e, int idx, int thr, int ratio)
{
    const int knee = e.pt[idx][1];
    int target = knee;
    if (ratio != 500) target = toInt32((double)knee - ((double)thr * 10.0) / (double)ratio - 0.5);
    if (target < -840) return -840;
    return target > 0 ? 0 : target;
}

// the graph was edited (points dragged, added or deleted): the threshold and the ratio are found from it again
inline void graphEdited(Settings& s)
{
    int idx = 0;
    s.threshold = thresholdAt(s.pt, s.count, &idx);
    Settings t = s;
    s.ratio = ratioOf(t, idx);
}

// the ratio slider (tenths, 10..500)
inline void setRatio(Settings& s, int ratio)
{
    s.ratio = ratio;
    Edit e(s);
    int idx = 0;
    const int thr = thresholdAt(e.pt, e.count, &idx);
    const int delta = lastLevelFor(e, idx, thr, ratio) - e.pt[e.count - 1][1];
    moveLast(e, idx, delta);
    e.store(s);
}

// the threshold slider (tenths of a dB): the knee moves and the ratio stays
inline void setThreshold(Settings& s, int threshold)
{
    const int delta = threshold - s.threshold;
    if (delta == 0) return;
    Edit e(s);
    moveKnee(e, delta);
    s.threshold = threshold;
    int idx = 0;
    const int thr = thresholdAt(e.pt, e.count, &idx);
    const int ratio = std::max(10, std::min(500, (int)s.ratio));
    const int d2 = lastLevelFor(e, idx, thr, ratio) - e.pt[e.count - 1][1];
    moveLast(e, idx, d2);
    e.store(s);
}

// the Reset Graph button
inline void resetGraph(Settings& s)
{
    const int pts[3][2] = {{-840, -840}, {-210, -210}, {0, 0}};
    std::memset(s.pt, 0, sizeof s.pt);
    std::memcpy(s.pt, pts, sizeof pts);
    s.count = 3;
    graphEdited(s);
}

// ------------------------------------------------------------------------------------------------------------ sound
class Dsp {
public:
    static const int kTable = 841;          // the gain at each tenth of a dB from 0 down to -84 dB

    void setFormat(int rate, int channels)
    {
        rate_ = rate > 0 ? rate : 44100;
        ch_ = std::max(1, std::min(2, channels));
        blk_ = toInt((double)rate_ * 0.02);
        if (blk_ < 1) blk_ = 1;
        for (int c = 0; c < 2; ++c) buf_[c].assign((size_t)blk_, 0.f);
        std::fill(table_, table_ + kTable, 0.0);        // a new state: the table starts empty
        fill_ = 0;
        started_ = false;
        restart_ = false;
        inited_ = false;
        dirty_ = true;
        fullReset();
        if (have_) init();
        inited_ = have_;
        dirty_ = false;
    }
    int block() const { return blk_; }
    int latency() const { return blk_; }
    int channels() const { return ch_; }

    // the settings the sound should use from the next call on; a change is taken up by applyChanges
    void setSettings(const Settings& s)
    {
        if (!have_ || s != set_) {
            set_ = s;
            dirty_ = true;
        }
        have_ = true;
    }
    // a jump in time (a discontinuity): the next call starts the effect again
    void restart() { restart_ = true; }
    bool bypass() const { return bypass_; }

    // The start of a call (what the plug-in does before it takes in the sound of a buffer): it reads its settings and
    // works its state out again - every call, since asking for the settings marks them as changed - which forgets the
    // loudest sample so far of the block being collected; after a discontinuity everything starts again.
    void applyChanges()
    {
        if (!have_) return;
        // (the state only has to be worked out again when something changed or when the graph does not reach all of the
        // table: the plug-in works what it finds there out once more every time)
        if (dirty_ || !inited_ || !covers()) {
            init();
            inited_ = true;
        }
        if (restart_) {
            fullReset();
            fill_ = 0;
        } else {
            for (int c = 0; c < 32; ++c) chan_[c].peak = 0;
        }
        dirty_ = false;
        restart_ = false;
        started_ = true;
    }

    // one frame of the sound; true when this frame completed a block (its processed sound is then in outAt)
    bool push(const float* in)
    {
        for (int c = 0; c < ch_; ++c) {
            const float x = in[c];
            buf_[c][(size_t)fill_] = x;
            double v = (double)x;
            if (v > chan_[c].peak) {
                chan_[c].peak = v;
            } else {
                v = -v;
                if (v > chan_[c].peak) chan_[c].peak = v;
            }
        }
        if (++fill_ < blk_) return false;
        apply();
        fill_ = 0;
        return true;
    }
    float outAt(int c, int i) const { return buf_[c][(size_t)i]; }

    // One call of the plug-in's Process: it takes the frames of 'in' (interleaved, ch frames of channels) until a block is
    // complete, hands the finished blocks out into 'out' (room for 'cap' frames) and stops when the finished sound that did
    // not fit is still waiting or the input is used up. 'consumed' and 'delivered' say how far it came. Call it again with
    // the rest of the input, as a host does. Every call begins with applyChanges.
    void process(const float* in, int inN, float* out, int cap, int* consumed, int* delivered)
    {
        applyChanges();
        int used = 0, given = 0;
        for (;;) {
            const int avail = (int)(pend_.size() / (size_t)ch_);
            const int d = std::min(cap - given, avail);
            if (d > 0) {
                std::copy(pend_.begin(), pend_.begin() + (size_t)d * ch_, out + (size_t)given * ch_);
                pend_.erase(pend_.begin(), pend_.begin() + (size_t)d * ch_);
                given += d;
            }
            if (!pend_.empty() || used >= inN) break;
            bool full = false;
            while (used < inN && !full) {
                full = push(in + (size_t)used * ch_);
                ++used;
            }
            if (full)
                for (int i = 0; i < blk_; ++i)
                    for (int c = 0; c < ch_; ++c) pend_.push_back(buf_[c][(size_t)i]);
        }
        *consumed = used;
        *delivered = given;
    }
    // the finished sound still waiting for room (frames)
    int pending() const { return (int)(pend_.size() / (size_t)ch_); }
    // a copy of the first frames of it (without taking them: the next call still gives them), for a host that needs the
    // sound before the plug-in's call would have handed it over; returns how many frames there were
    int peekPending(float* out, int frames) const
    {
        const int n = std::min(frames, pending());
        std::copy(pend_.begin(), pend_.begin() + (size_t)n * ch_, out);
        return n;
    }

    double gainOf(int c) const { return chan_[c].gain; }

    // the gain table (tests): the linear gain at -i/10 dB
    double tableAt(int i) const { return table_[i]; }

private:
    struct Chan {
        int flag = 0;               // 2 while the level is above the threshold (attack), else 0 (release)
        double gain = 1.0;          // the gain now (without the output gain)
        double peak = 0;            // the loudest sample of the block so far
    };
    Settings set_;
    bool have_ = false, dirty_ = false, inited_ = false, started_ = false, restart_ = false, bypass_ = false;
    int rate_ = 44100, ch_ = 2, blk_ = 882, fill_ = 0;
    std::vector<float> buf_[2];
    std::vector<float> pend_;
    Chan chan_[32];
    double table_[kTable];
    // the plug-in's state object
    double maxGain_ = 50.0, outGain_ = 1.0, thrHi_ = 0, thrLo_ = 0, attackCoef_ = 1.0, releaseCoef_ = 1.0;
    int attackFrames_ = 0, releaseFrames_ = 0, sync_ = 0;

    bool covers() const { return set_.pt[0][0] == -840 && set_.pt[set_.count - 1][0] == 0; }

    // cvttsd2si: truncation; out of range gives the "integer indefinite" value
    static int toInt(double x)
    {
        if (!(x > -2147483649.0 && x < 2147483648.0)) return (int)0x80000000u;
        return (int)x;
    }

    void fullReset()
    {
        for (int c = 0; c < 32; ++c) chan_[c] = Chan();
        pend_.clear();
    }

    // The level (in tenths of a dB) the sound has to reach for the attack to take over (SfGdyn 0x180041050): the
    // highest input level of a point that is the corner of a curve going from about 1:1 to flatter
    int kneeLevel() const
    {
        const Settings& s = set_;
        const int n = s.count;
        int best = -840, at = n - 2;
        for (int e = n - 2; e > 0; --e) {
            const double slope2 = (double)(s.pt[e + 1][1] - s.pt[e][1]) / (double)(s.pt[e + 1][0] - s.pt[e][0]);
            const double slope1 = (double)(s.pt[e][1] - s.pt[e - 1][1]) / (double)(s.pt[e][0] - s.pt[e - 1][0]);
            bool take = false;
            if (1.0 > slope2 && slope1 >= 0.9) take = true;
            else if (1.1 > slope2 && slope1 >= 1.1) take = true;
            if (take && s.pt[e][0] > best) {
                best = s.pt[e][0];
                at = e;
            }
        }
        return s.pt[at][0];
    }

    // the straight line between two table entries, as the plug-in fills it
    void interpolate(int a, int b)
    {
        int lo = a, hi = b;
        if (lo > hi) std::swap(lo, hi);
        const double span = (double)(hi - lo);
        if (span == 0.0) return;
        const double step = (table_[hi] - table_[lo]) / span;
        for (int k = lo + 1; k < hi; ++k) table_[k] = (double)(k - lo) * step + table_[lo];
    }

    // The settings made into the state (SfGdyn 0x180041350, 0x1800411a0)
    void init()
    {
        const Settings& s = set_;
        const double attackSecs = (double)s.attack / 10000.0, releaseSecs = (double)s.release / 10000.0;
        blk_ = toInt((double)rate_ * 0.02);
        attackFrames_ = toInt((double)rate_ * attackSecs);
        releaseFrames_ = toInt((double)rate_ * releaseSecs);
        thrLo_ = (double)kneeLevel();
        thrLo_ = std::pow(10.0, thrLo_ / 200.0);
        thrHi_ = thrLo_;
        if (releaseSecs > 0.02) releaseCoef_ = 1.0 - std::pow(0.1, 0.02 / releaseSecs);
        else releaseCoef_ = 1.0;
        if (attackSecs > 0.02) attackCoef_ = 1.0 - std::pow(0.1, 0.02 / attackSecs);
        else attackCoef_ = 1.0;
        maxGain_ = 50.0;
        sync_ = s.sync;
        outGain_ = std::pow(10.0, (double)s.outGain / 200.0);
        if (s.autoGain) outGain_ *= 0.98;
        bypass_ = s.bypass != 0;
        buildTable();
        for (int i = 0; i < kTable; ++i) {
            if (table_[i] == -840.0) table_[i] = 0.0;
            else table_[i] = std::pow(10.0, table_[i] / 200.0);
        }
    }

    // the table in tenths of a dB of gain (the output level less the input level), point by point
    void buildTable()
    {
        const Settings& s = set_;
        int best = -840;
        for (int k = 0; k + 1 < s.count; ++k) {
            const int x0 = s.pt[k][0], y0 = s.pt[k][1], x1 = s.pt[k + 1][0], y1 = s.pt[k + 1][1];
            best = std::max(best, y0);
            table_[-x0] = (double)(y0 - x0);
            best = std::max(best, y1);
            table_[-x1] = (double)(y1 - x1);
            if (y1 == -840 && y0 == -840) {
                table_[-x1] = -840.0;
                table_[-x0] = -840.0;
            }
            if (k == 0 && y0 == -840 && -200.0 > table_[-x1]) table_[-x0] = -840.0;
            interpolate(-x1, -x0);
        }
        if (best < 0 && s.autoGain)
            for (int i = 0; i < kTable; ++i) table_[i] -= (double)best;
    }

    // the processing of the block now in buf_ (SfGdyn 0x180041b40)
    void apply()
    {
        const int n = fill_;
        double all = 0;
        bool synced = false;
        if (sync_ != 0 && ch_ > 1) {
            synced = true;
            for (int c = 0; c < ch_; ++c)
                if (chan_[c].peak > all) all = chan_[c].peak;
        }
        const double keepAttack = 1.0 - attackCoef_, keepRelease = 1.0 - releaseCoef_;
        for (int c = 0; c < ch_; ++c) {
            Chan& ch = chan_[c];
            if (synced) ch.peak = all;
            const double peak = ch.peak;
            if (peak >= thrHi_) ch.flag = 2;
            else if (thrLo_ > peak) ch.flag = 0;
            // the level of the peak in tenths of a dB, and the gain the graph gives for it
            double lv;
            if (3.0517578125e-05 > peak) lv = -840.0;
            else lv = std::log10(peak) * 200.0;
            if (-840.0 > lv) lv = -840.0;
            else if (lv > 0.0) lv = 0.0;
            const int whole = toInt(lv);
            const int at = -whole;
            double target = table_[at];
            if (lv > -840.0) target += (table_[at + 1] - table_[at]) * ((double)whole - lv);
            // the gain moves towards it by a step of one pole per block, fast when above the threshold
            const double cur = ch.gain;
            double g;
            int ramp;
            if (ch.flag != 0) {
                g = target * attackCoef_ + cur * keepAttack;
                ramp = attackFrames_;
            } else {
                g = target * releaseCoef_ + cur * keepRelease;
                ramp = releaseFrames_;
            }
            if (g > maxGain_) g = maxGain_;
            double from = cur * outGain_;
            ch.gain = g;
            g = g * outGain_;
            if (0.0002 > g) {
                g = 0.0;
                ch.gain = 0.0;
            } else if (g > 5000.0) {
                g = 5000.0;
                ch.gain = 5000.0;
            }
            double step;
            if (ramp > n) {
                step = (g - from) / (double)n;
            } else if (ramp != 0) {
                step = (g - from) / (double)ramp;
            } else {
                from = g;
                step = 0.0;
            }
            float* b = buf_[c].data();
            double cg = from;
            for (int i = 0; i < n; ++i) {
                b[i] = (float)((double)b[i] * cg);
                if (step > 0.0) {
                    if (g > cg) cg += step;
                } else if (0.0 > step) {
                    if (cg > g) cg += step;
                }
            }
            ch.peak = 0;
        }
    }
};

}  // namespace veggd
}  // namespace reno
