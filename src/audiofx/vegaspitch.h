// VEGAS Pitch Shift (sfppack1's CSfPitchs), float32 stereo path: a windowed-sinc resampler (the pitch change) followed, for
// "Preserve duration", by an overlap-add time stretch that puts the length back. Translated function by function from the
// plug-in's engine (the names of the members say which field of its object they are, e.g. f1e8 = [this + 0x1e8]), so
// the output is the same samples. The engine is a stream: how many frames come out for the frames that go in depends on
// the mode (a few hundred milliseconds are held back), see PitchEngine for how Reno fits that into fixed blocks.
#pragma once
#include "vegaspack1.h"
#include "vegaspitchaa.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The "Mode" records of the plug-in (19 of 40 bytes); only the five numbers the engine uses.
struct PitchMode { int frameMs, searchMs, step, fadeMs, thresholdPct; };
inline const PitchMode& pitchMode(int i)
{
    static const PitchMode t[19] = {
        {300, 15, 2, 15, 2500}, {275, 22, 2, 20, 10000}, {150, 15, 2, 15, 1500}, {220, 15, 4, 15, 1500},
        {190, 15, 2, 15, 1500}, {150, 15, 3, 10, 1000},  {90, 15, 2, 30, 2500},  {78, 12, 2, 10, 2000},
        {80, 12, 4, 10, 2000},  {150, 25, 2, 30, 2500},  {130, 25, 3, 30, 2500}, {110, 25, 2, 30, 2500},
        {75, 10, 2, 10, 1000},  {90, 15, 2, 12, 2000},   {85, 15, 4, 12, 2000},  {95, 15, 8, 12, 2000},
        {50, 5, 2, 3, 1000},    {70, 9, 2, 8, 2000},     {50, 7, 2, 8, 1000}};
    return t[std::max(0, std::min(18, i))];
}

class PitchCore {
public:
    PitchCore()
    {
        std::memset(S_, 0, sizeof S_);
        scratch_.assign((size_t)2 * 0x800, 0.f);
        const double one = 1.0, init = 2646000.0;
        std::memcpy(S_ + 6, &one, 8);
        std::memcpy(S_ + 10, &init, 8);
        S_[2] = 2;
    }

    void setRate(uint32_t r)
    {
        if (r != rate_) needInit_ = 1;
        rate_ = r;
    }
    // The property setters of the plug-in: a changed value marks the engine as changed; accuracy, preserve duration,
    // mode and the anti-alias switch also make it set everything up again.
    void setSettings(const int32_t* n)
    {
        for (int i = 0; i < 12; ++i) {
            if (S_[i] == n[i]) continue;
            S_[i] = n[i];
            changed_ = 1;
            if (i == 2 || i == 3 || i == 4 || i == 8) needInit_ = 1;
        }
    }
    double ratio() const { double r; std::memcpy(&r, S_ + 6, 8); return r; }
    int frameSamples() const { return N_; }
    int halfFrame() const { return half_; }
    int hop() const { return H_; }
    double stepRatio() const { return f1e8_; }
    bool preserving() const { return f0b0_ != 0; }

    // 0x15d00: before a block is run (the anti-alias filter works on the input block itself).
    void prepare(int mode, float* in, int frames)
    {
        int32_t cur[12];
        const int chg = getSettings(cur);
        if ((chg || !ready_) && mode != 2) init(cur);
        if (f1e8_ > 1.1 && f0b4_) antiAlias(in, frames);
    }

    // 0x15680. inCnt: frames available in / frames used; outCnt: room for output / frames made. Result as the plug-in's.
    int run(int mode, const float* in, int* inCnt, float* out, int* outCnt)
    {
        const int ebx = mode == 2;
        f0ac_ = mode == 0;
        int inAvail = inCnt ? *inCnt : 0;
        const int cap0 = outCnt ? *outCnt : 0;
        int cap = cap0;
        int32_t cur[12];
        const int chg = getSettings(cur);
        if (f0ac_ && seen_) {
            if (reset() < 0) return -1;
        } else if ((chg || !ready_) && !ebx) {
            if (init(cur) < 0) return -1;
        }
        seen_ = 1;
        int esi, r12 = 0;
        if (f0b0_ == 0) {
            f0a8_ = ebx;
            if (ebx) {
                if (outCnt) *outCnt = 0;
                return 1;
            }
            resample(in, &inAvail, out, &cap);
            esi = cap;
        } else {
            int req = 0;
            if (!ebx && p29c_ == 0) {
                req = 0x800;
                resample(in, &inAvail, scratch_.data(), &req);
                s2d8_ += inAvail;
                s2d4_ = 0;
                s2d0_ = req;
            } else {
                inAvail = 0;
            }
            int rem = s2d0_ - s2d4_;
            if (rem == 0 && ebx) f0a8_ = 1;
            cap = cap0;
            const int r = sola(scratch_.data() + 2 * (size_t)s2d4_, &rem, out, &cap);
            esi = cap;
            s2dc_ += esi;
            r12 = r;
            s2d4_ += rem;
            if ((uint32_t)s2dc_ > (uint32_t)s2d8_ && 1.0 > f1e8_) {
                r12 = 1;
                esi += s2d8_ - s2dc_;
            } else if (r12 == 1 && (uint32_t)s2d8_ > (uint32_t)s2dc_ && esi == 0) {
                const uint32_t room = (uint32_t)s2d8_ - (uint32_t)s2dc_;
                esi = (int)((uint32_t)cap0 < room ? (uint32_t)cap0 : room);
                std::memset(out, 0, (size_t)esi * 2 * 4);
                s2dc_ += esi;
                r12 = (uint32_t)s2d8_ <= (uint32_t)s2dc_;
            }
        }
        if (inCnt) *inCnt = inAvail;
        if (outCnt) *outCnt = esi;
        return r12;
    }

private:
    int32_t S_[12];
    uint32_t rate_ = 48000;
    int changed_ = 1, needInit_ = 1, seen_ = 1, ready_ = 0;           // 0x60, 0x64, 0x6c, 0xa0
    int f0a8_ = 0, f0ac_ = 0, f0b0_ = 0, f0b4_ = 0;
    int outRate_ = 0, taps_ = 9, tapHalf_ = 4;                         // 0xb8, 0xc8, 0xcc
    std::vector<double> ring_[2];                                     // 0xd8
    std::vector<double> table_;                                       // 0x1d8
    double tabScale_ = 0, f1e8_ = 1.0, pos_ = 0, base_ = 0;           // 0x1e0, 0x1e8, 0x1f0, 0x1f8
    int outTotal_ = 0, ringIdx_ = 0, need_ = 0, used_ = 0, made_ = 0; // 0x200, 0x204, 0x208, 0x20c, 0x210
    // overlap-add stage
    int N_ = 0, half_ = 0, H_ = 0, F_ = 0, step_ = 0, range_ = 0;     // 0x248, 0x24c, 0x250, 0x254, 0x288, 0x28c
    float thr_ = 0;                                                   // 0x290
    std::vector<float> A_, B_, win_;                                  // 0x258, 0x260, 0x268
    int p294_ = 1, p29c_ = 0, in2a0_ = 0, fill2a4_ = 0, out2a8_ = 0, rd2ac_ = 0, end2b0_ = 0, rest2b4_ = 0, fl2b8_ = 0;
    std::vector<float> scratch_;                                      // 0x2c0
    int s2d0_ = 0, s2d4_ = 0, s2d8_ = 0, s2dc_ = 0;

    // 0x18e8f0
    int getSettings(int32_t* out)
    {
        std::memcpy(out, S_, sizeof S_);
        const int c = changed_;
        changed_ = 0;
        return c;
    }
    static double dblAt(const int32_t* s, int i) { double d; std::memcpy(&d, s + i, 8); return d; }

    // 0x1800149e0: the stream starts again (a discontinuity)
    int reset()
    {
        needInit_ = 1;
        fl2b8_ = 0;
        scratch_.assign((size_t)2 * 0x800, 0.f);
        s2d0_ = 0x800;
        int32_t cur[12];
        getSettings(cur);
        init(cur);
        p294_ = 1;
        f0a8_ = 0;
        outTotal_ = 0;
        s2d8_ = 0;
        s2dc_ = 0;
        ringIdx_ = 0;
        base_ = 0;
        pos_ = 0;
        need_ = 0;
        seen_ = 0;
        return 0;
    }

    // 0x180014310
    int init(const int32_t* s)
    {
        const int preserve = s[3], anti = s[8];
        const double ratio = dblAt(s, 6);
        f0b0_ = preserve;
        f0b4_ = anti;
        if (needInit_ == 0) {
            outRate_ = (int32_t)(int64_t)((double)rate_ / ratio);
            f1e8_ = (double)rate_ / (double)(uint32_t)outRate_;
            if (preserve) {
                if (0.5 > f1e8_) f1e8_ = 0.5;
                solaSetup(s);
            }
            base_ = 0;
            outTotal_ = 0;
            pos_ = 0;
            need_ = 0;
            if (f1e8_ > 1.1 && f0b4_) antiAliasDesign(0.5 / f1e8_);
            return 0;
        }
        outRate_ = (int32_t)(int64_t)((double)rate_ / ratio);
        const int acc = s[2];
        taps_ = acc == 1 ? 2 : (acc == 3 ? 15 : 9);
        tapHalf_ = taps_ / 2;
        ringIdx_ = 0;
        for (int c = 0; c < 2; ++c) ring_[c].assign((size_t)taps_, 0.0);
        tabScale_ = 4000.0 / (double)(tapHalf_ + 1);
        table_.assign(4001, 0.0);
        table_[0] = 1.0;
        if (taps_ == 2) {
            for (int i = 1; i <= 1999; ++i) table_[(size_t)i] = (double)(2000 - i) / 2000.0;
        } else {
            const double pi = 3.14159265356;
            const int ecx = tapHalf_ + 1;
            double last = 0;
            for (int i = 1; i < 4000; ++i) {
                const double x = (double)(ecx * i) / 4000.0 * pi;
                double w6 = x / (double)ecx + pi;
                double a = std::cos(w6 * 2.0) * 0.14128;
                double b = std::cos(w6) * 0.48829;
                double t1 = 0.35875 - b;
                double w8 = a + t1;
                double c = std::cos(w6 * 3.0) * 0.01168;
                w8 = w8 - c;
                const double sn = std::sin(x) / x;
                w8 = w8 * sn;
                table_[(size_t)i] = w8;
                last = w8;
            }
            table_[4000] = last;
        }
        f1e8_ = (double)rate_ / (double)(uint32_t)outRate_;
        int r14 = 0;
        if (preserve) {
            if (0.5 > f1e8_) f1e8_ = 0.5;
            r14 = solaSetup(s);
        }
        if (f1e8_ > 1.1 && f0b4_) antiAliasDesign(0.5 / f1e8_);
        f0a8_ = 0;
        outTotal_ = 0;
        s2d8_ = 0;
        s2dc_ = 0;
        ringIdx_ = 0;
        base_ = 0;
        pos_ = 0;
        need_ = 0;
        needInit_ = 0;
        p294_ = 1;
        ready_ = r14 >= 0;
        return r14;
    }

    // 0x1800128b0 (stereo, float)
    int solaSetup(const int32_t* s)
    {
        if (needInit_ == 0) {
            int r8 = (int)((double)half_ / f1e8_ + 0.5);
            if (r8 < 1) r8 = 1;
            else if (r8 > N_) r8 = N_;
            H_ = r8;
            rest2b4_ = N_ - r8;
            return 0;
        }
        const PitchMode& m = pitchMode(s[4]);
        N_ = (int)((uint32_t)((uint32_t)rate_ * (uint32_t)m.frameMs) / 1000u);
        A_.assign((size_t)2 * N_, 0.f);
        B_.assign((size_t)2 * N_, 0.f);
        win_.assign((size_t)N_, 1.0f);
        uint32_t f = (uint32_t)((uint32_t)m.fadeMs * (uint32_t)rate_) / 1000u;
        {
            const int lim = N_ / 2 - 1;
            F_ = (int)f > lim ? lim : (int)f;
        }
        if (F_ > 1) {
            const int half = N_ / 2;
            for (int i = 0; i < F_; ++i) {
                const float v = (float)i / (float)F_ * win_[(size_t)i];
                win_[(size_t)i] = v;
                win_[(size_t)(half + F_ - i)] = v;
            }
            for (int i = half + F_ + 1; i < N_; ++i) win_[(size_t)i] = 0.f;
        }
        half_ = N_ / 2;
        end2b0_ = half_;
        double r = (double)half_ / f1e8_ + 0.5;
        int r10 = (int)r;
        if (r10 < 1) r10 = 1;
        else if (r10 > N_) r10 = N_;
        H_ = r10;
        range_ = half_ - F_;
        const int edx = (int)((uint32_t)((uint32_t)m.searchMs * (uint32_t)rate_) / 1000u);
        if (range_ > edx) range_ = edx;
        step_ = m.step;
        if (rate_ < 32000u) {
            int e = (int)((uint32_t)((uint32_t)m.step * (uint32_t)rate_) / 44100u);
            if (e < 1) e = 1;
            step_ = e;
        }
        p29c_ = 0;
        fill2a4_ = 0;
        rd2ac_ = 0;
        ready_ = 1;
        rest2b4_ = N_ - r10;
        thr_ = (float)m.thresholdPct / 100.0f;
        return 0;
    }

    // 0x180013a80: the resampler. in: interleaved stereo float; *inCnt frames available -> used; out: *outCnt frames wanted -> made.
    void resample(const float* in, int* inCnt, float* out, int* outCnt)
    {
        used_ = 0;
        made_ = 0;
        if (*outCnt > 0) {
            for (;;) {
                fetch(in, inCnt);
                if (need_ != 0) break;
                double tmp[2] = {0.0, 0.0};
                int edi = taps_;
                int r11 = ringIdx_;
                double x4 = (double)tapHalf_ - (pos_ - base_);
                if (edi == 2) {
                    const double w = 1.0 - x4;
                    for (int c = 0; c < 2; ++c) tmp[c] = w * ring_[c][(size_t)r11];
                    r11 += 1;
                    if (r11 > 1) r11 = 0;
                    for (int c = 0; c < 2; ++c) tmp[c] = x4 * ring_[c][(size_t)r11] + tmp[c];
                } else if (edi > 0) {
                    int r9 = r11;
                    const int last = taps_ - 1;
                    while (edi > 0) {
                        const double a = std::fabs(x4) * tabScale_;
                        const int ecx = (int)a;
                        double d = table_[(size_t)ecx + 1] - table_[(size_t)ecx];
                        const double frac = a - (double)ecx;
                        d = d * frac;
                        d = d + table_[(size_t)ecx];
                        for (int c = 0; c < 2; ++c) tmp[c] = d * ring_[c][(size_t)r9] + tmp[c];
                        r11 += 1;
                        r9 += 1;
                        if (r11 > last) {
                            r11 = 0;
                            r9 = 0;
                        }
                        x4 += (0.0 - 1.0);
                        --edi;
                    }
                }
                // put
                out[2 * (size_t)made_] = (float)tmp[0];
                out[2 * (size_t)made_ + 1] = (float)tmp[1];
                ++made_;
                ++outTotal_;
                pos_ = (double)outTotal_ * f1e8_;
                const double x5 = pos_ - base_;
                need_ = (int)x5;
                if (need_ < 0) need_ = 0;
                if (!(made_ < *outCnt)) break;
            }
        }
        *inCnt = used_;
        *outCnt = made_;
    }

    // 0x180013770
    void fetch(const float* in, const int* inCnt)
    {
        const int base = 2 * used_;
        int idx = base;
        if (f0ac_) {
            const int e = tapHalf_ + 1;
            f0ac_ = 0;
            need_ = e;
            ringIdx_ = e;
            base_ = base_ - (double)e;
        }
        while (need_ > 0 && used_ < *inCnt) {
            if (--ringIdx_ < 0) ringIdx_ = taps_ - 1;
            --need_;
            for (int c = 0; c < 2; ++c) ring_[c][(size_t)ringIdx_] = (double)in[idx++];
            base_ += 1.0;
            ++used_;
        }
    }

    // 0x1800170b0 (two channels)
    int sola(const float* in, int* inCnt, float* out, int* outCnt)
    {
        in2a0_ = 0;
        out2a8_ = 0;
        if (*inCnt == 0 && p29c_ == 0 && fl2b8_ != 0) {
            *outCnt = 0;
            return 1;
        }
        for (;;) {
            if (p29c_ == 1) {
                solaOut(out, *outCnt);
                if (rd2ac_ != end2b0_) break;
                rd2ac_ = 0;
                if (f0a8_ == 0) fill2a4_ = rest2b4_;
                std::memmove(A_.data(), A_.data() + 2 * (size_t)H_, (size_t)(N_ - H_) * 2 * 4);
                std::memmove(B_.data(), B_.data() + 2 * (size_t)half_, (size_t)F_ * 2 * 4);
                std::memset(B_.data() + 2 * (size_t)F_, 0, (size_t)(N_ - F_) * 2 * 4);
                p29c_ = 0;
            }
            solaIn(in, *inCnt);
            if (f0a8_ != 0) {
                int ecx = fill2a4_;
                if (ecx < half_) {
                    if (fl2b8_ != 0) break;
                    fl2b8_ = 1;
                    if (ecx <= 0) break;
                    std::memset(A_.data() + 2 * (size_t)ecx, 0, (size_t)(N_ - ecx) * 2 * 4);
                    ecx = fill2a4_;
                    end2b0_ = ecx;
                }
                ecx -= H_;
                fill2a4_ = ecx;
            }
            if (fill2a4_ != N_ && f0a8_ == 0) break;
            const int off = findOffset();
            overlapAdd(off);
            p29c_ = 1;
        }
        *inCnt = in2a0_;
        *outCnt = out2a8_;
        return 0;
    }

    // 0x180016f80
    void solaIn(const float* in, int inCnt)
    {
        uint32_t ebx = (uint32_t)inCnt - (uint32_t)in2a0_;
        const uint32_t eax = (uint32_t)(N_ - fill2a4_);
        if (ebx >= eax) ebx = eax;
        std::memcpy(A_.data() + 2 * (size_t)fill2a4_, in + 2 * (size_t)in2a0_, (size_t)ebx * 2 * 4);
        in2a0_ += (int)ebx;
        fill2a4_ += (int)ebx;
    }
    // 0x180017010
    void solaOut(float* out, int outCnt)
    {
        if (end2b0_ == 0) return;
        uint32_t r11 = (uint32_t)outCnt - (uint32_t)out2a8_;
        const uint32_t eax = (uint32_t)(end2b0_ - rd2ac_);
        if (r11 >= eax) r11 = eax;
        std::memcpy(out + 2 * (size_t)out2a8_, B_.data() + 2 * (size_t)rd2ac_, (size_t)r11 * 2 * 4);
        out2a8_ += (int)r11;
        rd2ac_ += (int)r11;
    }

    // 0x180016250: where in the new frame the sound fits the tail of the last one best
    int findOffset() const
    {
        const float* A = A_.data();
        const float* B = B_.data();
        const int esi = 2 * step_;
        float e3 = 0.f, e4 = 0.f;
        const int R = range_;
        if (R != 0) {
            int edx = 0, idx = 0;
            do {
                const float l = B[idx], r = B[idx + 1];
                idx += esi;
                edx += step_;
                e3 += r * r;
                e4 += l * l;
            } while ((uint32_t)edx < (uint32_t)R);
        }
        e4 *= thr_;
        e3 *= thr_;
        float best = 0.f;
        int edi = 0;
        if (R != 0) {
            int ecx = 0, r11 = 0;
            do {
                const float* a = A + r11;
                const float* b = B;
                float x2 = 0.f, x1 = 0.f;
                int r8 = 0;
                do {
                    float t = b[0] + a[0];
                    t = t * t;
                    x2 += t;
                    float u = b[1] + a[1];
                    u = u * u;
                    x1 += u;
                    b += esi;
                    a += esi;
                    r8 += step_;
                } while ((uint32_t)r8 < (uint32_t)R);
                const float tot = x1 + x2;
                if (x2 > e4 && x1 > e3) return ecx;
                if (tot > best) {
                    best = tot;
                    edi = ecx;
                }
                ecx += step_;
                r11 += esi;
            } while ((uint32_t)ecx < (uint32_t)R);
        }
        return edi;
    }

    // 0x1800165b0
    void overlapAdd(int off)
    {
        float* B = B_.data();
        const float* A = A_.data() + 2 * (size_t)off;
        const float* w = win_.data();
        if (p294_ != 0) {
            for (int i = 0; i < F_; ++i) {
                B[2 * i] = A[2 * i] + B[2 * i];
                B[2 * i + 1] = A[2 * i + 1] + B[2 * i + 1];
            }
            p294_ = 0;
        } else {
            for (int i = 0; i < F_; ++i) {
                B[2 * i] = A[2 * i] * w[i] + B[2 * i];
                B[2 * i + 1] = A[2 * i + 1] * w[i] + B[2 * i + 1];
            }
        }
        const int ebx = half_ - F_;
        std::memmove(B + 2 * (size_t)F_, A + 2 * (size_t)F_, (size_t)ebx * 8);
        float* d = B + 2 * (size_t)half_;
        const float* s = A + 2 * (size_t)half_;
        const float* ww = w + half_;
        for (int i = 0; i < F_; ++i) {
            d[2 * i] = s[2 * i] * ww[i] + d[2 * i];
            d[2 * i + 1] = s[2 * i + 1] * ww[i] + d[2 * i + 1];
        }
    }

    // The anti-alias filter (ratio above 1.1 with the box ticked): see vegaspitchaa.h
    void antiAliasDesign(double x) { aa_.design(x, 2); }
    void antiAlias(float* in, int frames) { aa_.apply(in, frames, 2); }
    aa::Filter aa_;
};


// How many frames the sound is held back with these settings. The overlap-add stage (Preserve duration) waits for a
// whole frame of the resampled sound and puts out half a frame at a time, so the output trails the input by up to
// (frame length x step of the resampler) frames; the plug-in is reported to the host with that delay and keeps the
// sound in step by the same amount.
inline int pitchLatencyFor(const int32_t* s, uint32_t rate)
{
    if (rate == 0) return 0;
    double ratio;
    std::memcpy(&ratio, s + 6, 8);
    if (!(ratio > 0.0) || !std::isfinite(ratio)) ratio = 1.0;
    const int32_t ob = (int32_t)(int64_t)((double)rate / ratio);
    double eff = ob > 0 ? (double)rate / (double)(uint32_t)ob : 1.0;
    if (eff < 0.5) eff = 0.5;
    const PitchMode& m = pitchMode(s[4]);
    const double n = (double)(uint32_t)((uint32_t)rate * (uint32_t)m.frameMs / 1000u);
    return (int)std::min(std::ceil(eff * n) + 1024.0, 4000000.0);
}

// The effect inside Reno's fixed blocks: the plug-in's stream engine is fed each block the way the filter's Transform
// does and what it puts out is queued; the block gets the next frames of the queue (after as many silent frames as the
// latency, so the sound comes out in step with the input; short of sound, silence).
// Without "Preserve duration" the engine only resamples: the sound comes out shorter (pitched up) or longer (down) than
// it went in, which a track's effect chain cannot do (it would need the sound ahead of the playing position). In a track's chain Reno runs
// the engine as if the box were ticked, so the pitch changes and the length stays; the setting itself is kept and saved.
// Apply Non-Real-Time Event FX gives the whole sound to whole(), which really changes the length.
class PitchEngine : public Engine {
public:
    void setup(const int32_t* s, uint32_t rate, bool keep) override
    {
        core_.setRate(rate);
        int32_t t[12];
        std::memcpy(t, s, sizeof t);
        std::memcpy(orig_, s, sizeof orig_);
        t[3] = 1;
        core_.setSettings(t);
        if (!keep) {
            disc_ = true;
            q_.clear();
            head_ = 0;
            q_.assign((size_t)2 * (size_t)pitchLatencyFor(s, rate), 0.f);
        }
    }
    int latency(const int32_t* s, uint32_t rate) const override { return pitchLatencyFor(s, rate); }
    // Without "Preserve duration" (and a real pitch change) the sound comes out shorter or longer than it went in: only a
    // whole sound can be treated so (Apply Non-Real-Time Event FX), see whole().
    bool changesLength(const int32_t* s) const override
    {
        double ratio;
        std::memcpy(&ratio, s + 6, 8);
        return s[3] == 0 && std::isfinite(ratio) && std::fabs(ratio - 1.0) > 1e-9;
    }
    // The whole sound the way the filter gets it, with the settings as they are (Preserve duration off: the resampler's
    // output as it is): blocks of input, then the end of the stream (asked for what the engine still holds until it says
    // it is done). Nothing is cut or padded.
    bool whole(const float* inL, const float* inR, int64_t frames, std::vector<float>& outL, std::vector<float>& outR) override
    {
        outL.clear();
        outR.clear();
        core_.setSettings(orig_);
        const int block = 4096;
        std::vector<float> in((size_t)2 * block), ob((size_t)2 * kChunk);
        int mode = 0;
        auto take = [&](int outC) {
            for (int k = 0; k < outC; ++k) {
                outL.push_back(ob[(size_t)2 * k]);
                outR.push_back(ob[(size_t)2 * k + 1]);
            }
        };
        for (int64_t at = 0; at < frames; at += block) {
            const int n = (int)std::min<int64_t>(block, frames - at);
            for (int k = 0; k < n; ++k) {
                in[(size_t)2 * k] = inL[at + k];
                in[(size_t)2 * k + 1] = inR[at + k];
            }
            core_.prepare(mode, in.data(), n);
            int rem = n;
            const float* p = in.data();
            for (;;) {
                int inC = rem, outC = kChunk;
                core_.run(mode, p, &inC, ob.data(), &outC);
                mode = 1;
                take(outC);
                if (inC >= rem) break;
                rem -= inC;
                p += 2 * (size_t)inC;
            }
        }
        for (int k = 0; k < 100000; ++k) {
            int inC = 0, outC = kChunk;
            core_.prepare(2, nullptr, 0);
            const int r = core_.run(2, nullptr, &inC, ob.data(), &outC);
            take(outC);
            if (r == 1 && outC == 0) break;
        }
        return true;
    }
    void process(float* left, float* right, int frames) override
    {
        if (frames <= 0) return;
        in_.resize((size_t)2 * (size_t)frames);
        for (int k = 0; k < frames; ++k) {
            in_[(size_t)2 * k] = left[k];
            in_[(size_t)2 * k + 1] = right[k];
        }
        ob_.resize((size_t)2 * kChunk);
        int mode = disc_ ? 0 : 1;
        disc_ = false;
        core_.prepare(mode, in_.data(), frames);
        int rem = frames;
        const float* p = in_.data();
        for (;;) {
            int inC = rem, outC = kChunk;
            core_.run(mode, p, &inC, ob_.data(), &outC);
            if (outC > 0) q_.insert(q_.end(), ob_.begin(), ob_.begin() + (size_t)2 * outC);
            if (inC >= rem) break;
            rem -= inC;
            p += 2 * (size_t)inC;
            mode = 1;
        }
        const size_t have = (q_.size() - head_) / 2;
        const size_t take = std::min<size_t>((size_t)frames, have);
        for (size_t k = 0; k < take; ++k) {
            left[k] = q_[head_ + 2 * k];
            right[k] = q_[head_ + 2 * k + 1];
        }
        for (size_t k = take; k < (size_t)frames; ++k) left[k] = right[k] = 0.f;
        head_ += 2 * take;
        // a long stream that runs ahead of the input (pitched down without keeping the duration) is cut to a few seconds
        const size_t keepMax = (size_t)2 * 48000 * 20;
        if (q_.size() - head_ > keepMax) head_ = q_.size() - keepMax;
        if (head_ > (1u << 20)) {
            q_.erase(q_.begin(), q_.begin() + (std::ptrdiff_t)head_);
            head_ = 0;
        }
    }

private:
    static constexpr int kChunk = 8192;
    PitchCore core_;
    int32_t orig_[12] = {};
    bool disc_ = true;
    std::vector<float> in_, ob_, q_;
    size_t head_ = 0;
};

inline void pitchDerive(int32_t* s, int slot)
{
    if (slot != 0 && slot != 1) return;
    const double r = std::pow(2.0, ((double)s[1] / 1000.0 + (double)s[0]) / 12.0);
    std::memcpy(s + 6, &r, 8);
}

inline const Info& pitchInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.pitchshift";
        i.name = "Pitch Shift";
        i.clsid = "ed1b4100-93be-11d0-aebc-00a0c9053912";
        const uint8_t g[16] = {0x03, 0x41, 0x1b, 0xed, 0xbe, 0x93, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 12;
        i.bypassSlot = -1;
        i.derive = pitchDerive;
        i.params = {
            {"Semitones", -50, 50, 0, "", 0, 1, Num, "", 0, false},
            {"Cents", -100, 100, 0, "", 1, 10, Num, "", 1, false},
            {"Accuracy", 1, 3, 2, "", 2, 1, Num, "", 0, false},
            {"Anti-alias Filter", 0, 1, 0, "", 8, 1, Bool, "", 0, false},
            {"Preserve Duration", 0, 1, 0, "", 3, 1, Bool, "", 0, false},
            {"Mode", 0, 18, 0, "", 4, 1, Choice,
             "A01. Music 1 (minimum flange, may echo)|A02. Music 2|A03. Music 3 (less echo)|A04. Music 4 (fast, good for bass)|"
             "A05. Music 5|A06. Music 6|A07. Speech 1|A08. Speech 2|A09. Speech 3 (fast)|A10. Solo instruments 1|"
             "A11. Solo instruments 2|A12. Solo instruments 3|A13. Solo instruments 4 (less echo)|A14. Solo instruments 5|"
             "A15. Solo instruments 6|A16. Solo instruments 7 (fast)|A17. Drums, unpitched (minimum echo)|A18. Drums (better for toms)|"
             "A19. Drums (tiny echo)", 0, false},
            {"Initial Time", 0, 225306900.0, 2646000.0, "", 10, 1, Dbl, "", 0, false},
        };
        auto preset = [](const char* name, int semis, double ratio) {
            std::vector<int32_t> v(12, 0);
            v[0] = semis;
            v[2] = 2;
            const double ini = 2646000.0;
            std::memcpy(&v[6], &ratio, 8);
            std::memcpy(&v[10], &ini, 8);
            return Preset{name, v};
        };
        i.presets = {
            preset("(Default)", 0, 1.0),
            preset("Fifth down", -7, 0.66742),
            preset("Fifth up", 7, 1.4983),
            preset("Third down", -4, 0.7937),
            preset("Third up", 4, 1.25992),
        };
        i.make = [] { return std::unique_ptr<Engine>(new PitchEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
