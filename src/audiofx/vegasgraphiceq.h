// VEGAS's Graphic EQ DirectX effect (sfppack2_x64.dll, CLSID 8b7226ee-4584-11d1-b4cb-00a0c9270a10): its saved settings,
// presets and its processing, worked out from the DLL's own code. Reno's built-in Graphic EQ uses the same settings and
// saved form, so a project's VEGAS Graphic EQ loads into it and it saves back as VEGAS Graphic EQ.
//
// The sound is an FIR filter by overlap-add. The filter is designed from the envelope (up to 64 points, x in
// 1/1000 octaves above 20 Hz, y a slider position where 100 is 3 dB and 200 is 6 dB): a magnitude response from the
// points, an impulse response from it (inverse FFT, window), and its spectrum. The sound comes in blocks of N/2 frames
// (N is 256, 1024 or 4096 by the accuracy setting), each is zero padded, transformed, multiplied by the filter's
// spectrum, transformed back and added to the tail of the last block. The output gain is applied at the end. The
// 10 and 20 band pages of the plug-in only edit the envelope (a band moves a trapezoid of it), see bandToPoints().
#pragma once
#include "../core/base.h"
#include "vegasfft.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace reno {
namespace veggeq {

const char* const kClsid = "8b7226ee-4584-11d1-b4cb-00a0c9270a10";
const char* const kDxUid = "dshow:{8B7226EE-4584-11D1-B4CB-00A0C9270A10}";

const int kBands = 20;
const int kMaxPoints = 64;

// The plug-in's own layout (what it saves, after its length and a GUID): 151 32-bit words.
struct Settings {
    int32_t accuracy = 1;               // 0: 256 point filter (low), 1: 1024 (medium), 2: 4096 (high)
    int32_t band[kBands] = {};          // the band faders, tenths of a dB, -480 (-Inf.) .. 240
    int32_t pt[kMaxPoints][2] = {{0, 0}, {10000, 0}};  // the envelope: x 0..10000 (1/1000 octave above 20 Hz), y -500 (-Inf.) .. 400
    int32_t count = 2;                  // points in use, 2..64
    int32_t outGain = 0;                // tenths of a dB, -600 .. 200
};
static_assert(sizeof(Settings) == 0x25c, "VEGAS Graphic EQ's settings are 0x25c bytes");

// The GUID VEGAS Graphic EQ writes before its settings (and only reads this one).
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0xee, 0x2a, 0x72, 0x8b, 0x84, 0x45, 0xd1, 0x11, 0xb4, 0xcb, 0x00, 0xa0, 0xc9, 0x27, 0x0a, 0x10};
    return g;
}

inline void sanitize(Settings& s)
{
    s.accuracy = std::max(0, std::min(2, (int)s.accuracy));
    for (int32_t& b : s.band) b = std::max(-480, std::min(240, (int)b));
    for (int k = 0; k < kMaxPoints; ++k) {
        s.pt[k][0] = std::max(0, std::min(10000, (int)s.pt[k][0]));
        s.pt[k][1] = std::max(-500, std::min(400, (int)s.pt[k][1]));
    }
    s.count = std::max(2, std::min(kMaxPoints, (int)s.count));
    s.outGain = std::max(-600, std::min(200, (int)s.outGain));
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x270), the GUID, the settings.
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
    if (b.size() < 20 + sizeof(Settings)) return false;
    uint32_t len = 0;
    std::memcpy(&len, b.data(), 4);
    if (len != 0x270 || std::memcmp(b.data() + 4, persistGuid(), 16) != 0) return false;
    Settings s;
    std::memcpy(&s, b.data() + 20, sizeof(Settings));
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    const char* name;
    Settings s;
};

inline Settings withPoints(std::initializer_list<int> xy)
{
    Settings s;
    s.count = (int)xy.size() / 2;
    int k = 0;
    for (int v : xy) {
        s.pt[k / 2][k % 2] = v;
        ++k;
    }
    return s;
}

// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", withPoints({0, 0, 10000, 0})},
        {"Default all parameters", withPoints({0, 0, 10000, 0})},
        {"Roll-off frequencies below 120 Hz", withPoints({0, -500, 2673, 0, 10000, 0})},
        {"Boost bass around 80 Hz by 6 dB", withPoints({0, 0, 1000, 0, 1250, 202, 2750, 202, 3000, 0, 10000, 0})},
        {"Boost high frequencies above 7 kHz by 6 dB", withPoints({0, 0, 8250, 0, 8375, 202, 10000, 202})},
        {"Boost mid-range around 1 kHz by 6 dB", withPoints({0, 0, 5132, 0, 5382, 202, 5882, 202, 6132, 0, 10000, 0})},
        {"Boost upper three octaves", withPoints({0, 0, 7013, 0, 10000, 268})},
        {"Gentle high-frequency boost", withPoints({0, 0, 5000, 0, 10000, 150})},
        {"Notch frequencies around 450 Hz", withPoints({0, 0, 4000, 0, 4250, -500, 4750, -500, 5000, 0, 10000, 0})},
    };
    return list;
}

// ------------------------------------------------------------------------------------------ the envelope
// The slider position of a graph point (-500 is -Inf., 100 is 3 dB, 200 is 6 dB, 400 is 24 dB) as a linear gain
inline double posGain(int y)
{
    if (y == -500) return 0.0;
    const double s = y < 0 ? -1.0 : 1.0;
    const int a = y < 0 ? -y : y;
    double e;
    if ((unsigned)(y + 100) <= 200u) {
        const double p = std::pow(2.0, (double)a / 100.0);
        e = (p - 1.0) * 3.0 * s;
    } else {
        const double p = std::pow(2.0, ((double)a - 100.0) / 100.0);
        e = p * 3.0 * s;
    }
    return std::pow(10.0, e / 20.0);
}

// The level of a slider position in dB (-1000 for -Inf.)
inline double posDb(int y)
{
    if (y <= -500) return -1000.0;
    const double s = y < 0 ? -1.0 : 1.0;
    const int a = y < 0 ? -y : y;
    if ((unsigned)(y + 100) <= 200u) return (std::pow(2.0, (double)a / 100.0) - 1.0) * 3.0 * s;
    return std::pow(2.0, ((double)a - 100.0) / 100.0) * 3.0 * s;
}

// The slider position of a level in tenths of a dB (the plug-in's 0x180009a00); -480 is -Inf.
inline int posOfTenths(int v)
{
    if (v == -480) return v - 20;
    const double x = (double)v / 10.0;
    const int sign = x >= 0.0 ? 1 : -1;
    const double ax = std::fabs(x);
    double r;
    if (x >= -3.0 && 3.0 >= x) r = std::log(ax / 3.0 + 1.0) * 100.0 / std::log(2.0) + 0.5;
    else r = std::log(ax / 3.0) * 100.0 / std::log(2.0) + 100.5;
    return (int)r * sign;
}

// x (1/1000 octave above 20 Hz) as a frequency
inline double freqOf(int x) { return 20.0 * std::pow(2.0, (double)x / 1000.0); }

// The plug-in's clean up of an envelope after an edit (0x18000c8e0): sorted by x, in range, and without points that add
// nothing (the middle of three in a row at the same level, or a point within 100 of its neighbour in x and 10 in y).
inline void cleanPoints(int32_t (*p)[2], int* count)
{
    int n = *count;
    for (int top = n - 1; top > 0; --top)
        for (int j = 0; j < top; ++j)
            if (p[j][0] > p[j + 1][0]) std::swap(p[j], p[j + 1]);
    for (int i = 0; i < n; ++i) {
        p[i][1] = std::max(-500, std::min(400, (int)p[i][1]));
        p[i][0] = std::max(0, std::min(10000, (int)p[i][0]));
    }
    int i = 1;
    while (n > 2 && i < n - 1) {
        bool remove = false;
        if (p[i - 1][1] == p[i][1] && p[i][1] == p[i + 1][1]) remove = true;
        else if (p[i][0] - p[i - 1][0] < 100 && std::abs(p[i][1] - p[i - 1][1]) < 10) remove = true;
        if (remove) {
            for (int k = i; k + 1 < n; ++k) p[k][0] = p[k + 1][0], p[k][1] = p[k + 1][1];
            --n;
        } else {
            ++i;
        }
    }
    *count = n;
}

// What a band fader does to the envelope (the plug-in's page code, 0x18000ca90): the band's trapezoid, a flat top from
// c-w to c+w at the fader's level and slopes down to the curve as it was at c-2w and c+2w, where c = 500 * band and w
// is 250 on the 20 band page (all bands) and 500 on the 10 band page (the odd bands). The points between c-2w and c+2w
// are replaced. 'p' has room for kMaxPoints + 8 points; the result is cleaned up and cut to kMaxPoints.
inline void bandToPoints(int32_t (*p)[2], int* count, int band, int tenths, int w)
{
    int n = *count;
    for (int top = n - 1; top > 0; --top)
        for (int j = 0; j < top; ++j)
            if (p[j][0] > p[j + 1][0]) std::swap(p[j], p[j + 1]);
    const int c = band * 500;
    const int xL2 = c - 2 * w, xL1 = c - w, xR1 = c + w, xR2 = c + 2 * w;
    int r10 = 0, r8 = 0, r11 = n - 1, e2 = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i][0] < xL2) r10 = i;
        if (p[i][0] == xL2 - w) r8 = i;
    }
    for (int i = n - 1; i >= 0; --i) {
        if (p[i][0] > xR2) r11 = i;
        if (p[i][0] == xR2 + w) e2 = i;
    }
    const bool needLeft = !(r8 > 0), needRight = !(e2 > 0);
    double slope = 0.0;
    if (band != 0 && r10 + 1 < n) slope = (double)(p[r10 + 1][1] - p[r10][1]) / (double)(p[r10 + 1][0] - p[r10][0]);
    const int yLeft = (int)((double)(xL2 - p[r10][0]) * slope) + p[r10][1];
    int yRight = 0;
    if (r11 >= 1) {
        const double sl = (double)(p[r11][1] - p[r11 - 1][1]) / (double)(p[r11][0] - p[r11 - 1][0]);
        yRight = (int)(sl * (double)(xR2 - p[r11 - 1][0])) + p[r11 - 1][1];
    }
    // the points in the span go (never the first or the last)
    {
        int last = n - 1;
        for (int i = n - 1; i > 0; --i) {
            if (p[i][0] >= xL2 && p[i][0] <= xR2 && i != last) {
                for (int k = i; k < last; ++k) p[k][0] = p[k + 1][0], p[k][1] = p[k + 1][1];
                --n;
                --last;
            }
        }
    }
    const int yGain = posOfTenths(tenths);
    auto put = [&](int x, int y) {
        int at = 0;
        while (at < n && p[at][0] < x) ++at;
        if (at < n && p[at][0] == x) {
            p[at][1] = y;
            return;
        }
        for (int k = n; k > at; --k) p[k][0] = p[k - 1][0], p[k][1] = p[k - 1][1];
        p[at][0] = x;
        p[at][1] = y;
        ++n;
    };
    if (band != 0) put(xL1, yGain);
    if (band != kBands - 1) put(xR1, yGain);
    if (needLeft && band != 0) put(xL2, yLeft);
    if (needRight && band != kBands - 1) put(xR2, yRight);
    if (band == 0 || (w == 250 && band == 1)) p[0][1] = p[1][1];
    else if (band == kBands - 1) p[n - 1][1] = p[n - 2][1];
    cleanPoints(p, &n);
    *count = std::min(n, kMaxPoints);
}

// ------------------------------------------------------------------------------------------ the filter design
// Graphic EQ: the magnitude response of the envelope (the plug-in's 0x180006530 and the two before it).
// A straight line between bins a and b (float arithmetic for the steps, as in the plug-in)
inline void interp(double* m, int a, int b)
{
    if (a > b) std::swap(a, b);
    const double d = (double)(b - a);
    if (d == 0.0) return;
    const float slope = (float)((m[b] - m[a]) / d);
    for (int k = a + 1; k < b; ++k) m[k] = (double)((float)(k - a) * slope) + m[a];
}

// 'n' bins from 0 Hz to half the sample rate in m (room for 2n); pts = (x, y) pairs
inline void response(double* m, int n, const int32_t (*pts)[2], int count, int rate)
{
    const double perHz = (double)n / ((double)rate * 0.5);
    auto bin = [&](int x) {
        const double b = std::pow(2.0, (double)x / 1000.0) * 20.0 * perHz + 0.5;
        return b >= (double)(2 * n - 1) ? 2 * n - 1 : (int)b;
    };
    auto f2d = [](double v) { return (double)(float)v; };
    {
        const int i0 = bin(pts[0][0]);
        const double g = posGain(pts[0][1]);
        m[0] = f2d(g);
        m[i0] = f2d(g);
        interp(m, 0, i0);
    }
    {
        const int last = count - 1;
        const int i1 = std::min(n - 1, bin(pts[last][0]));
        const double g = posGain(pts[last][1]);
        m[i1] = f2d(g);
        m[n - 1] = f2d(g);
        interp(m, i1, n - 1);
    }
    for (int k = 0; k + 1 < count; ++k) {
        const int xa = pts[k][0], ya = pts[k][1];
        const double dx = (double)(pts[k + 1][0] - xa) / 10.0, dy = (double)(pts[k + 1][1] - ya) / 10.0;
        for (int j = 0; j < 10; ++j) {
            const int ia = std::min(n - 1, bin((int)((double)j * dx + 0.5) + xa));
            const int ib = std::min(n - 1, bin((int)((double)(j + 1) * dx + 0.5) + xa));
            const double ga = posGain((int)((double)j * dy + 0.5) + ya);
            const double gb = posGain((int)((double)(j + 1) * dy + 0.5) + ya);
            m[ia] = f2d(ga);
            m[ib] = f2d(gb);
            interp(m, ia, ib);
        }
    }
}

// The complex double FFT that turns the response into an impulse response (sfppack2 0x180077d10: decimation in
// frequency, scaled by (float)(1/n)); d is n complex numbers (re, im), n = 1 << log2n
inline void fftcInv(double* d, int log2n, std::vector<double>& tw)
{
    const int n = 1 << log2n, half = n / 2;
    tw.assign(2 * (size_t)(half > 1 ? half - 1 : 1), 0.0);
    {
        const double s = std::atan(1.0) * 4.0 / (double)half;
        const double c0 = std::cos(s), s0 = std::sin(s);
        double c = c0, sn = s0;
        for (int i = 0; i < half - 1; ++i) {
            tw[2 * (size_t)i] = (double)(float)c;
            tw[2 * (size_t)i + 1] = (double)(float)sn;
            const double nc = c0 * c - s0 * sn;
            const double ns = sn * c0 + s0 * c;
            c = nc;
            sn = ns;
        }
    }
    int step = 1;
    int m = n;
    for (int pass = 0; pass < log2n; ++pass) {
        m /= 2;
        for (int i = 0; i < n; i += 2 * m) {
            double* a = d + 2 * (size_t)i;
            double* b = d + 2 * (size_t)(i + m);
            const double sr = b[0] + a[0], si = b[1] + a[1];
            b[0] = a[0] - b[0];
            b[1] = a[1] - b[1];
            a[0] = sr;
            a[1] = si;
        }
        for (int j = 1; j < m; ++j) {
            const double wr = tw[2 * (size_t)(step * j - 1)], wi = tw[2 * (size_t)(step * j - 1) + 1];
            for (int i = j; i < n; i += 2 * m) {
                double* a = d + 2 * (size_t)i;
                double* b = d + 2 * (size_t)(i + m);
                const double sr = b[0] + a[0], si = b[1] + a[1];
                const double dr = a[0] - b[0], di = a[1] - b[1];
                b[0] = wr * dr - wi * di;
                b[1] = wi * dr + wr * di;
                a[0] = sr;
                a[1] = si;
            }
        }
        step *= 2;
    }
    {
        int j = 0;
        for (int i = 1; i < n - 1; ++i) {
            int k = half;
            if (j >= k) {
                do {
                    j -= k;
                    k /= 2;
                } while (k <= j);
            }
            j += k;
            if (i < j) {
                std::swap(d[2 * i], d[2 * j]);
                std::swap(d[2 * i + 1], d[2 * j + 1]);
            }
        }
    }
    const double sc = (double)(float)(1.0 / (double)n);
    for (int i = 0; i < 2 * n; ++i) d[i] *= sc;
}

// Magnitude response -> windowed impulse response (sfppack2 0x1800068b0). m: 2n doubles; the first n are the response,
// on return the first taps+1 are the windowed impulse response. x and tw are scratch.
inline void impulse(double* m, int n, int taps, std::vector<double>& x, std::vector<double>& tw)
{
    const int n2 = 2 * n;
    int lg = 0;
    while ((1 << lg) < n2) ++lg;
    x.assign(2 * (size_t)n2, 0.0);
    for (int i = 0; i < n; ++i) {
        x[2 * (size_t)i] = m[i];
        x[2 * (size_t)i + 1] = 0.0;
    }
    for (int k = 0; k < n; ++k) {
        x[2 * (size_t)(n + k)] = x[2 * (size_t)(n - 1 - k)];
        x[2 * (size_t)(n + k) + 1] = -x[2 * (size_t)(n - 1 - k) + 1];
    }
    fftcInv(x.data(), lg, tw);
    for (int i = 0; i < n2; ++i) m[i] = (i == 0 || i == n2 - 1) ? x[2 * (size_t)i] : x[2 * (size_t)i] * 2.0;
    for (int j = taps; j >= 0; --j) {
        const double c = std::cos((double)j * 3.141592653589793 / (double)taps);
        m[taps - j] = (0.54 - c * 0.46) * m[taps - j];
    }
}

inline int filterSize(int accuracy) { return accuracy == 0 ? 0x100 : accuracy == 1 ? 0x400 : 0x1000; }

// The processing, in the plug-in's own order of operations and its own arithmetic (float samples; the filter and the
// gain in double).
class Dsp {
public:
    // The format is set once (the plug-in's set-media-type): rate and channel count (1..2 here; the DLL takes more).
    void setFormat(double rate, int channels)
    {
        rate_ = std::max(8000, (int)(rate + 0.5));
        ch_ = std::max(1, std::min(kMaxCh, channels));
        built_ = false;
        applyChanges();
    }
    void setSettings(const Settings& s)
    {
        Settings t = s;
        sanitize(t);
        if (!hasSet_ || std::memcmp(&t, &set_, sizeof t) != 0) {
            set_ = t;
            hasSet_ = true;
            dirty_ = true;
        }
    }
    int block() const { return hop_; }
    int latency() const { return hop_; }

    // The start of a stream, or after a jump in time: nothing of the sound before stays.
    void restart()
    {
        if (!built_) return;
        for (int c = 0; c < kMaxCh; ++c) {
            std::fill(ovl_[c].begin(), ovl_[c].end(), 0.f);
            std::fill(buf_[c].begin(), buf_[c].end(), 0.f);
        }
        fill_ = 0;
    }

    // Takes the settings that were changed since the last call (the plug-in does this at each buffer it gets): the
    // filter is designed again, and the next block that is complete is made with it.
    void applyChanges()
    {
        if (!hasSet_) {
            set_ = Settings();
            hasSet_ = true;
            dirty_ = true;
        }
        if (built_ && !dirty_) return;
        const int n = filterSize(set_.accuracy);
        const bool resize = !built_ || n != n_;
        n_ = n;
        hop_ = n / 2;
        taps_ = hop_ - 1;
        design();
        if (resize) {
            for (int c = 0; c < kMaxCh; ++c) {
                buf_[c].assign((size_t)n_, 0.f);
                ovl_[c].assign((size_t)taps_, 0.f);
                out_[c].assign((size_t)hop_, 0.f);
            }
            fill_ = 0;
        }
        built_ = true;
        dirty_ = false;
    }

    // One frame in. Returns true once a block (block() frames) is complete and processed: its samples are then in
    // outAt(c, 0..block()-1); the next push starts the next block.
    bool push(const float* frame)
    {
        if (!built_) return false;
        for (int c = 0; c < ch_; ++c) buf_[c][(size_t)fill_] = frame[c];
        if (++fill_ < hop_) return false;
        processBlock();
        fill_ = 0;
        return true;
    }
    int blockFrames() const { return hop_; }
    float outAt(int c, int i) const { return out_[c][(size_t)i]; }

private:
    static constexpr int kMaxCh = 2;
    int rate_ = 48000, ch_ = 2;
    int n_ = 0x400, hop_ = 0x200, taps_ = 0x1ff;
    int fill_ = 0;
    bool built_ = false, dirty_ = false, hasSet_ = false;
    Settings set_;
    double gain_ = 1.0;
    std::vector<double> filter_;                 // the filter's spectrum (n_ doubles)
    std::vector<double> m_, x_, tw_;             // design scratch
    std::vector<float> buf_[kMaxCh], ovl_[kMaxCh], out_[kMaxCh];

    // the plug-in's design (0x180006c50)
    void design()
    {
        gain_ = std::pow(10.0, (double)set_.outGain / 200.0);
        m_.assign(2 * (size_t)n_, 0.0);
        response(m_.data(), n_, set_.pt, set_.count, rate_);
        impulse(m_.data(), n_, taps_, x_, tw_);
        filter_.assign((size_t)n_, 0.0);
        for (int j = 0; j < taps_; ++j) filter_[(size_t)j] = (double)n_ * m_[(size_t)j];
        vegfft::rfftDfwd(filter_.data(), n_, 1.0);
    }

    // the spectrum times the filter's (0x18008c110): bins 0 and n/2 are real, the others complex
    void multiply(float* b) const
    {
        const double* f = filter_.data();
        b[0] = (float)((double)b[0] * f[0]);
        b[1] = (float)((double)b[1] * f[1]);
        for (int k = 1; k < n_ / 2; ++k) {
            const double re = (double)b[2 * k], im = (double)b[2 * k + 1];
            const double fr = f[2 * k], fi = f[2 * k + 1];
            const double nre = re * fr - im * fi;
            const double nim = im * fr + re * fi;
            b[2 * k] = (float)nre;
            b[2 * k + 1] = (float)nim;
        }
    }

    void processBlock()
    {
        for (int c = 0; c < ch_; ++c) {
            float* b = buf_[c].data();
            float* o = ovl_[c].data();
            std::fill(b + hop_, b + n_, 0.f);
            vegfft::rfftF(b, n_ / 2, true, 1.0f);
            multiply(b);
            vegfft::rfftF(b, n_ / 2, false, 1.0f);
            for (int i = 0; i < taps_; ++i) {
                b[i] = b[i] + o[i];
                o[i] = b[hop_ + i];
            }
            for (int i = 0; i < hop_; ++i) out_[c][(size_t)i] = (float)((double)b[i] * gain_);
        }
    }
};

}  // namespace veggeq
}  // namespace reno
