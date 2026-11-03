// VEGAS Timecode (Vfx1.ofx, com.vegascreativesoftware:timecode): the time string, the text box's layout and the kernel's rule, re-implemented from what its
// render function (0x18057b310 on the CPU, 0x18057ec80 for the OpenCL path VEGAS takes) and the time formatter (0x1806ddc70, frame counts 0x18071e720) do,
// read from the user's own VEGAS install; not a fit. No dependencies, so a test program can include it alone.
//
// What VEGAS does:
//  - The frame's time (100 ns ticks, snapped to the project's frame grid) is turned into text by the format (the choice maps to an internal id; 12 formats:
//    h:m:s.ms, seconds, h:m:s.ff, absolute frames "%.3f", feet+frames 16mm / 35mm, and the SMPTE counts 23.976 / 24 / 25 / 29.97 ND / 29.97 drop / 30).
//  - A font (Arial, weight 700, cell height = min(width, height) / 12, the Width / Height settings as the glyph matrix's x / y scale) is asked, through GDI's
//    GetGlyphOutline in GRAY8 (65 levels), for the 16 glyphs "0123456789;:.,+-". Each gives a bitmap and its metrics: black box (bw x bh), origin (ox, oy: the left
//    side bearing and the height of the top above the baseline) and the advance. GRAY8 levels are scaled by 4 and cut to 255.
//  - H = the largest (bh + oy) of the 16, maxOy = the largest oy, D = bh - oy of the "0" (see layout()), W = the advances of the string added.
//    Left = width/10 | (width - W)/2 | width - width/10 - W; the baseline's row (from the bottom) = H/2 + height/10 | H/4 + height/2 | height - height/10.
//  - The text is laid in a byte bitmap Wb x Hb (Wb = W + H/2, Hb = H/4 + D + maxOy), glyph after glyph from H/4 in (each overwriting the box of the one before),
//    and the kernel writes grey (g, g, g, alpha 1) inside the rectangle that starts one row and one column inside the bitmap, whose origin is
//    (Left - H/4, height - H/8 - baseline - D), rows counted from the bottom of the picture; the rest of the picture is unchanged.
// What this file cannot do is GDI: the glyph shapes and their metrics come from a font model (Reno's own) except on Windows, where timecodeFx asks GDI itself.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace reno {
namespace ofx {
namespace vfxtc {

// ---- the time string ---------------------------------------------------------------------------------------------------------------------------------
// cvttsd2si rounded by the "add 1.5 * 2^52" trick VEGAS's compiler emitted: the nearest whole number, ties to even
inline long long nearest(double v) { return std::isfinite(v) && std::fabs(v) < 2.0e15 ? (long long)std::nearbyint(v) : 0; }

// The frame count to timecode (0x18071e720). mode: 1 = 30 fps, 2 = 29.97 drop frame, 3 = 25, 4 = 24, 5 = 29.97 non-drop (counted at 30), 6 = 23.976 (counted at 24).
// Out: hours, minutes, seconds, frames.
inline void frameTimecode(int mode, uint32_t n, int out[4])
{
    uint32_t fps = 30, perHour = 108000, perMin = 1800, wrap = 2592000;
    if (mode == 3) { fps = 25; perHour = 90000; perMin = 1500; wrap = 2160000; }
    else if (mode == 4 || mode == 6) { fps = 24; perHour = 86400; perMin = 1440; wrap = 2073600; }
    if (mode == 2) {
        n %= 2589408u;                          // 24 hours of drop-frame counts
        // the count with the dropped numbers put back, by trial: how many numbers were dropped before it, until that stops changing
        uint32_t last = 0;
        for (int guard = 0; guard < 64; ++guard) {
            const uint32_t q = n / 108000u;
            uint32_t dropped = q * 108u;
            uint32_t mins = (n - q * 108000u) / 1800u;
            if (mins) dropped += (mins - mins / 10u) * 2u;
            if (dropped == last) break;
            n += dropped - last;
            last = dropped;
        }
    }
    n %= wrap;
    const uint32_t h = n / perHour; n -= h * perHour;
    const uint32_t m = n / perMin; n -= m * perMin;
    const uint32_t s = n / fps;
    uint32_t f = n - s * fps;
    if (mode == 2 && f < 2 && s == 0 && m != (m / 10u) * 10u) f = 2;      // the numbers 0 and 1 do not exist at the start of a minute that is not a tenth
    out[0] = (int)h; out[1] = (int)m; out[2] = (int)s; out[3] = (int)f;
}

// The time as hours, minutes, seconds and "frames" of 'rate' per second (0x1806ddaa0, the part for no SMPTE mode): the time is moved on by half a frame, so the
// frames are rounded to the nearest.
inline void clockTimecode(long long ticks, double rate, int out[4])
{
    double t = (double)ticks * 1e-7;
    if (rate > 0.01) t += 0.5 / rate;
    const double hrs = std::floor(t / 3600.0);
    t -= hrs * 3600.0;
    const double mins = std::floor(t / 60.0);
    t -= mins * 60.0;
    const double secs = std::floor(t);
    t -= secs;
    long long fr = nearest(t * rate - 0.499999985);
    if (fr < 0) fr = 0;
    out[0] = (int)((long long)hrs % 24); out[1] = (int)mins; out[2] = (int)secs; out[3] = (int)fr;
}

// The format choice (0..11, VEGAS's list order) -> the formatter's internal id
inline int internalFormat(int choice)
{
    static const int ids[12] = {1, 2, 3, 4, 0xf, 0x10, 0x12, 0xa, 9, 6, 8, 7};
    return ids[choice < 0 ? 0 : choice > 11 ? 11 : choice];
}

// The frame's time snapped to the grid of 'rate' (0x18057b55e): the frame count rounded as the code does, then back to ticks.
inline long long snapTicks(long long ticks, double rate)
{
    if (!(rate > 0) || !(rate < 1.0e6)) return ticks;
    const long long frame = nearest((double)ticks * 1e-7 * rate + 1e-5 - 0.499999985);
    double v = (double)frame * 1e7 / rate;
    v = rate > 200.0 ? v + 1.5e-8 : v + 1e-5 - 0.499999985;
    return nearest(v);
}

// The string a format makes of the time (ticks from the start of the clip) at the project's frame rate 'rate'.
inline std::string text(int choice, long long ticks, double rate)
{
    char buf[96];
    const int id = internalFormat(choice);
    const bool neg = ticks < 0;
    if (neg) ticks = -ticks;
    ticks = snapTicks(ticks, rate);
    int t[4] = {0, 0, 0, 0};
    switch (id) {
    case 1: clockTimecode(ticks, 1000.0, t); std::snprintf(buf, sizeof buf, "%02u:%02u:%02u.%03u", (unsigned)t[0], (unsigned)t[1], (unsigned)t[2], (unsigned)t[3]); break;
    case 2: std::snprintf(buf, sizeof buf, "%.3f", (double)ticks * 1e-7); break;
    case 3: clockTimecode(ticks, rate, t); std::snprintf(buf, sizeof buf, "%02u:%02u:%02u.%02u", (unsigned)t[0], (unsigned)t[1], (unsigned)t[2], (unsigned)t[3]); break;
    case 4: std::snprintf(buf, sizeof buf, "%.3f", (double)ticks * 1e-7 * rate + 1e-5); break;
    case 0xf:
    case 0x10: {
        const long long fr = nearest((double)ticks * 1e-7 * rate + 1e-5 - 0.499999985);
        const long long per = id == 0xf ? 40 : 16;
        std::snprintf(buf, sizeof buf, "%lld+%lld", fr / per, fr % per);
        break;
    }
    default: {
        // the SMPTE counts: the frame number = ticks x rate / 1e7 (+ 5e-6), then the timecode of that number
        static const double rates[7] = {0, 30.0, 30000.0 / 1001.0, 25.0, 24.0, 30000.0 / 1001.0, 24000.0 / 1001.0};
        const int mode = id == 7 ? 1 : id == 8 ? 2 : id == 9 ? 3 : id == 0xa ? 4 : id == 6 ? 5 : 6;
        double f = (double)ticks * rates[mode] / 1e7 + 5e-6;
        if (!(f >= 0 && f < 4.0e9)) f = 0;
        frameTimecode(mode, (uint32_t)f, t);
        std::snprintf(buf, sizeof buf, mode == 2 ? "%02u:%02u:%02u;%02u" : "%02u:%02u:%02u:%02u", (unsigned)t[0], (unsigned)t[1], (unsigned)t[2], (unsigned)t[3]);
    }
    }
    return neg ? std::string("-") + buf : std::string(buf);
}

// ---- glyphs and the layout -----------------------------------------------------------------------------------------------------------------------------
struct Glyph {
    int bw = 0, bh = 0;             // the black box
    int ox = 0, oy = 0;             // left of the box from the pen, top of the box above the baseline
    int adv = 0;                    // the pen's step
    std::vector<uint8_t> g;         // bw x bh bytes, top row first: min(255, 4 * the GRAY8 level)
};
struct Font { Glyph slot[16]; };

// "0123456789;:.,+-": the slot of a character (0x18057a150: a digit, else the first of ";:.,+-" it equals, else the last)
inline int slotOf(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    static const char* const rest = ";:.,+-";
    for (int i = 0; i < 6; ++i)
        if (rest[i] == c) return 10 + i;
    return 15;
}

struct Layout {
    bool ok = false;
    int ox = 0, oy = 0;             // the bitmap's origin in the picture (x from the left, y rows from the bottom)
    int wb = 0, hb = 0;             // the bitmap's size; the kernel shows its rows 1..hb-1 and columns 1..wb-1
    int width = 0;                  // W: the string's advances added
    int H = 0, maxOy = 0, D = 0;
    std::vector<uint8_t> bits;      // wb x hb, row 0 at the bottom
};

inline int cdiv(int a, int b) { return a / b; }                 // (C division: towards zero, as the compiled code does)

// The layout of 0x18057ec80 (and the CPU copy 0x18057b310); wd x ht = the picture, halign / valign the 0..2 choices.
inline Layout layout(const Font& f, const std::string& s, int wd, int ht, int halign, int valign)
{
    Layout L;
    if (wd <= 0 || ht <= 0) return L;
    int maxOy = f.slot[0].oy, D = f.slot[0].bh - f.slot[0].oy, H = 0;
    for (int i = 1; i < 16; ++i) {
        maxOy = std::max(maxOy, f.slot[i].oy);
        if (D < 0) D = f.slot[i].bh;                            // (the code's odd rule: a negative start is replaced by the next glyph's height)
    }
    for (int i = 0; i < 16; ++i) H = std::max(H, f.slot[i].bh + f.slot[i].oy);
    long long W = 0;
    for (char ch : s) W += f.slot[slotOf(ch)].adv;
    if (W > 100000) return L;
    const int w = (int)W;
    const int left = halign == 0 ? cdiv(wd, 10) : halign == 1 ? cdiv(wd, 2) - cdiv(w, 2) : wd - cdiv(wd, 10) - w;
    const int base = valign == 0 ? cdiv(ht, 10) + cdiv(H, 2) : valign == 1 ? cdiv(H, 4) + cdiv(ht, 2) : ht - cdiv(ht, 10);
    const int pad = cdiv(H, 4), h8 = cdiv(H, 8);
    L.width = w; L.H = H; L.maxOy = maxOy; L.D = D;
    L.ox = left - pad;
    L.oy = ht - h8 - base - D;
    L.wb = cdiv(H, 2) + w;
    L.hb = pad + D + maxOy;
    if (L.wb <= 0 || L.hb <= 0 || (long long)L.wb * L.hb > 64LL * 1024 * 1024) { L.wb = L.hb = 0; return L; }
    L.bits.assign((size_t)L.wb * L.hb, 0);
    int penX = pad;
    for (char ch : s) {
        const Glyph& g = f.slot[slotOf(ch)];
        const int x0 = penX + g.ox, y0 = g.oy - g.bh + h8 + D;      // the glyph's left column and bottom row in the bitmap
        penX += g.adv;
        for (int i = 0; i < g.bh; ++i) {
            const int y = y0 + i;
            if (y < 0 || y >= L.hb) continue;
            const uint8_t* src = &g.g[(size_t)(g.bh - 1 - i) * g.bw];      // the bottom row of the glyph goes to the lowest row
            for (int j = 0; j < g.bw; ++j) {
                const int x = x0 + j;
                if (x < 0 || x >= L.wb) continue;
                L.bits[(size_t)y * L.wb + x] = src[j];
            }
        }
    }
    L.ok = true;
    return L;
}

// The kernel's test and value for the picture's pixel (x, yFromTop): -1 outside the shown rectangle, else the byte of the bitmap.
inline int boxValue(const Layout& L, int ht, int x, int yFromTop)
{
    const int y = ht - 1 - yFromTop;
    if (!L.ok || y - L.oy >= L.hb || y - L.oy <= 0 || x - L.ox <= 0 || x - L.ox >= L.wb) return -1;
    return L.bits[(size_t)(y - L.oy) * L.wb + (x - L.ox)];
}

// ---- Reno's own font model (not GDI) ----------------------------------------------------------------------------------------------------------------------
// The same 16 glyphs as shapes on a small grid, area-sampled into the box the metrics give. GDI's real shapes and hinting are not reproduced.
inline void putShape(Glyph& g, const char* const* rows, int nr, int nc)
{
    g.g.assign((size_t)std::max(0, g.bw * g.bh), 0);
    for (int y = 0; y < g.bh; ++y)
        for (int x = 0; x < g.bw; ++x) {
            const double y0 = (double)y / g.bh * nr, y1 = (double)(y + 1) / g.bh * nr, x0 = (double)x / g.bw * nc, x1 = (double)(x + 1) / g.bw * nc;
            double cov = 0;
            for (int ry = (int)std::floor(y0); ry < nr && ry < y1; ++ry)
                for (int rx = (int)std::floor(x0); rx < nc && rx < x1; ++rx)
                    if (rows[ry][rx] == '1')
                        cov += (std::min<double>(y1, ry + 1) - std::max<double>(y0, ry)) * (std::min<double>(x1, rx + 1) - std::max<double>(x0, rx));
            const double a = cov / (((double)nr / g.bh) * ((double)nc / g.bw));
            g.g[(size_t)y * g.bw + x] = (uint8_t)std::min(255, 4 * (int)std::floor(std::min(1.0, a) * 64.0 + 0.5));
        }
}

// What the model assumes of the font: 'hinted' (the matrix is the identity: GDI hints, advances are whole pixels cut down) or not (the outlines are scaled).
// GUESS: the em of the unhinted case (GDI picked a smaller size than the hinted one with any other matrix: 2/3 of the cell height at a plain x scale, about
// 0.70 with a y scale) is a value read off captures of one frame size, not from the code, and it was not constant there; the metrics of those cases are approximate.
inline Font modelFont(int cellHeight, double sx, double sy)
{
    static const char* const dig[10][5] = {
        {"111", "101", "101", "101", "111"}, {"010", "110", "010", "010", "111"}, {"111", "001", "111", "100", "111"}, {"111", "001", "111", "001", "111"},
        {"101", "101", "111", "001", "001"}, {"111", "100", "111", "001", "111"}, {"111", "100", "111", "101", "111"}, {"111", "001", "001", "001", "001"},
        {"111", "101", "111", "101", "111"}, {"111", "101", "111", "001", "111"}};
    static const char* const colon[1] = {"1"};
    cellHeight = std::max(1, cellHeight);
    sx = std::isfinite(sx) ? std::min(8.0, std::max(0.05, sx)) : 1.0;
    sy = std::isfinite(sy) ? std::min(8.0, std::max(0.05, sy)) : 1.0;
    const bool hinted = std::fabs(sx - 1.0) < 1e-9 && std::fabs(sy - 1.0) < 1e-9;
    double em, ex, ey;                                          // the em in pixels, and the scale each way
    if (hinted) { em = std::floor((double)cellHeight * 2048.0 / 2288.0 + 0.5); ex = ey = 1.0; }
    else { em = (double)cellHeight * (std::fabs(sy - 1.0) < 1e-9 ? 2.0 / 3.0 : 0.70); ex = sx; ey = sy; }
    auto adv = [&](double em_) { return hinted ? (int)std::floor(em_ * ex) : (int)std::floor(em_ * ex + 0.5); };
    auto rnd = [](double v) { return (int)std::floor(v + 0.5); };
    Font F;
    const int digH = std::max(1, rnd(0.716 * em * ey)), digAdv = std::max(1, adv(0.556 * em)), digBW = std::max(1, std::min(digAdv, rnd(0.45 * em * ex)));
    const int punAdv = std::max(1, adv(0.278 * em)), colAdv = hinted ? punAdv : std::max(1, adv(0.333 * em)), dot = std::max(1, rnd(0.15 * em * ey)), dotW = std::max(1, std::min(punAdv, rnd(0.15 * em * ex)));
    const int plusAdv = std::max(1, adv(0.584 * em));
    for (int d = 0; d < 10; ++d) {
        Glyph& g = F.slot[d];
        g.bw = digBW; g.bh = digH; g.oy = digH; g.adv = digAdv; g.ox = std::max(0, (digAdv - digBW + 1) / 2);
        putShape(g, dig[d], 5, 3);
    }
    auto dotGlyph = [&](Glyph& g, int top, int bh, bool tail, bool upper, bool lower, bool colon) {
        g.bw = dotW; g.bh = bh; g.oy = top; g.adv = colon ? colAdv : punAdv; g.ox = std::max(0, (g.adv - dotW + 1) / 2);
        g.g.assign((size_t)g.bw * g.bh, 0);
        for (int y = 0; y < g.bh; ++y)
            for (int x = 0; x < g.bw; ++x) {
                const bool up = upper && y < dot, lo = lower && y >= (tail ? g.bh - 1 - dot : g.bh - dot) && y < (tail ? g.bh - 1 : g.bh);
                const bool tl = tail && y == g.bh - 1 && x >= g.bw / 2;
                g.g[(size_t)y * g.bw + x] = (up || lo || tl) ? 255 : 0;
            }
    };
    const int upTop = std::max(dot + 1, rnd(0.55 * em * ey));
    dotGlyph(F.slot[11], upTop, upTop, false, true, true, true);              // ':'
    dotGlyph(F.slot[10], upTop, upTop + 1, true, true, true, true);           // ';' (the lower dot with a tail)
    dotGlyph(F.slot[12], dot, dot, false, false, true, false);                 // '.'
    dotGlyph(F.slot[13], dot, dot + 2, true, false, true, false);              // ','
    {
        Glyph& g = F.slot[14];                                          // '+'
        g.adv = plusAdv; g.bw = std::max(1, std::min(plusAdv, digBW)); g.bh = std::max(1, rnd(0.42 * em * ey)); g.oy = std::max(g.bh, rnd(0.55 * em * ey));
        g.ox = std::max(0, (plusAdv - g.bw + 1) / 2);
        g.g.assign((size_t)g.bw * g.bh, 0);
        const int vw = std::max(1, rnd(0.15 * em * ex)), cx = (g.bw - vw) / 2, cy = g.bh / 2;
        for (int y = 0; y < g.bh; ++y)
            for (int x = 0; x < g.bw; ++x) g.g[(size_t)y * g.bw + x] = ((x >= cx && x < cx + vw) || y == cy) ? 255 : 0;
    }
    {
        Glyph& g = F.slot[15];                                          // '-'
        g.adv = punAdv; g.bw = std::max(1, std::min(punAdv, rnd(0.25 * em * ex))); g.bh = dot; g.oy = std::max(dot, rnd(0.33 * em * ey));
        g.ox = std::max(0, (punAdv - g.bw + 1) / 2);
        g.g.assign((size_t)g.bw * g.bh, 255);
    }
    (void)colon;
    return F;
}

}  // namespace vfxtc
}  // namespace ofx
}  // namespace reno
