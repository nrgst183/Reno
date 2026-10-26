// Loudness metering: see loudness.h.
#include "loudness.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace reno {

static const double kPi = 3.14159265358979323846;

void LoudnessMeter::reset(int sampleRate)
{
    rate_ = sampleRate > 0 ? sampleRate : 48000;
    // The K-weighting filters, worked out for this sample rate from BS.1770's analogue prototypes (at 48 kHz they
    // are the coefficients the standard lists).
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / rate_);
        const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        for (Biquad& b : shelf_) {
            b = Biquad();
            b.b0 = (Vh + Vb * K / Q + K * K) / a0;
            b.b1 = 2.0 * (K * K - Vh) / a0;
            b.b2 = (Vh - Vb * K / Q + K * K) / a0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / rate_);
        const double a0 = 1.0 + K / Q + K * K;
        for (Biquad& b : pass_) {
            b = Biquad();
            b.b0 = 1.0;
            b.b1 = -2.0;
            b.b2 = 1.0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    samples_ = 0;
    subLen_ = std::max(1, (int)std::lround(rate_ * 0.1));
    subFill_ = 0;
    subSum_ = 0;
    subs_.clear();
    blocks_.clear();
    shortBlocks_.clear();
    maxMomentary_ = maxShortTerm_ = kSilence;
    // The true-peak interpolator: a windowed sinc low pass at the original Nyquist frequency, 4 phases.
    const int L = 4, N = L * tapsPerPhase_;
    taps_.assign((size_t)N, 0.0);
    for (int n = 0; n < N; ++n) {
        const double x = (n - (N - 1) / 2.0) / L;
        const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (n + 0.5) / N);       // Hann
        taps_[(size_t)n] = sinc * w;
    }
    for (int k = 0; k < L; ++k) {
        double s = 0;
        for (int j = 0; j < tapsPerPhase_; ++j) s += taps_[(size_t)(j * L + k)];
        if (std::fabs(s) > 1e-12)
            for (int j = 0; j < tapsPerPhase_; ++j) taps_[(size_t)(j * L + k)] /= s;
    }
    for (auto& h : hist_) h.assign((size_t)tapsPerPhase_, 0.0);
    for (auto& h : histF_) h.assign((size_t)tapsPerPhase_, 0.0);
    for (int k = 0; k < 6; ++k) dcX_[k] = dcY_[k] = chPeak_[k] = curPeak_[k] = lastPeak_[k] = 0;
    histPos_ = 0;
    peak_ = 0;
}

double LoudnessMeter::loudnessOf(double ms) const
{
    if (!(ms > 0)) return kSilence;
    return std::max(kSilence, -0.691 + 10.0 * std::log10(ms));
}

// The peak of one side's newest sample and the three points between it and the one before, oversampled.
double LoudnessMeter::peakOversampled(int ch, float x, std::vector<double>* hist)
{
    std::vector<double>& h = hist ? *hist : hist_[ch];
    h[(size_t)histPos_] = x;
    double m = 0;
    const int L = 4;
    for (int k = 0; k < L; ++k) {
        double y = 0;
        for (int j = 0; j < tapsPerPhase_; ++j) {
            const int idx = (histPos_ - j + tapsPerPhase_) % tapsPerPhase_;
            y += taps_[(size_t)(j * L + k)] * h[(size_t)idx];
        }
        m = std::max(m, std::fabs(y));
    }
    return m;
}

// One channel's true peak at this sample: the sample itself and the oversampled points before it.
double LoudnessMeter::peakOf(int ch, float x)
{
    double m = std::max((double)std::fabs(x), peakOversampled(ch, x));
    if (blocking_) {
        const double R = 1.0 - 2.0 * kPi * 10.0 / rate_;
        const double y = x - dcX_[ch] + R * dcY_[ch];
        dcX_[ch] = x;
        dcY_[ch] = y;
        m = std::max(m, peakOversampled(ch, (float)y, &histF_[ch]));
    }
    chPeak_[ch] = std::max(chPeak_[ch], m);
    curPeak_[ch] = std::max(curPeak_[ch], m);
    return m;
}

void LoudnessMeter::processChannels(const float* const* ch, int channels, int frames)
{
    if (channels <= 2) {
        process(ch && channels > 0 ? ch[0] : nullptr, ch && channels > 1 ? ch[1] : nullptr, frames);
        return;
    }
    static const double kWeight[6] = {1.0, 1.0, 1.0, 0.0, 1.41, 1.41};
    const int n = std::min(6, channels);
    for (int i = 0; i < frames; ++i) {
        double sum = 0;
        for (int c = 0; c < n; ++c) {
            const float x = ch[c] ? ch[c][i] : 0.f;
            if (c != 3) peak_ = std::max(peak_, peakOf(c, x));
            const double k = pass_[c].run(shelf_[c].run(x));
            sum += kWeight[c] * k * k;
        }
        histPos_ = (histPos_ + 1) % tapsPerPhase_;
        subSum_ += sum;
        ++samples_;
        if (++subFill_ >= subLen_) endSubBlock();
    }
}

void LoudnessMeter::process(const float* left, const float* right, int frames)
{
    for (int i = 0; i < frames; ++i) {
        const float l = left ? left[i] : 0.f, r = right ? right[i] : l;
        // true peak (the samples themselves count too: the interpolator rounds a little)
        peak_ = std::max(peak_, peakOf(0, l));
        peak_ = std::max(peak_, peakOf(1, r));
        histPos_ = (histPos_ + 1) % tapsPerPhase_;
        const double kl = pass_[0].run(shelf_[0].run(l));
        const double kr = pass_[1].run(shelf_[1].run(r));
        subSum_ += kl * kl + kr * kr;
        ++samples_;
        if (++subFill_ >= subLen_) endSubBlock();
    }
}

void LoudnessMeter::endSubBlock()
{
    for (int k = 0; k < 6; ++k) {
        lastPeak_[k] = curPeak_[k];
        curPeak_[k] = 0;
    }
    subs_.push_back(subSum_ / subFill_);
    subSum_ = 0;
    subFill_ = 0;
    while (subs_.size() > 30) subs_.pop_front();
    if (subs_.size() >= 4) {
        double s = 0;
        for (size_t k = subs_.size() - 4; k < subs_.size(); ++k) s += subs_[k];
        blocks_.push_back(s / 4.0);
        maxMomentary_ = std::max(maxMomentary_, loudnessOf(s / 4.0));
    }
    if (subs_.size() >= 30) {
        double s = 0;
        for (double v : subs_) s += v;
        shortBlocks_.push_back(s / 30.0);
        maxShortTerm_ = std::max(maxShortTerm_, loudnessOf(s / 30.0));
    }
}

double LoudnessMeter::momentary() const
{
    if (subs_.empty()) return kSilence;
    const size_t n = std::min<size_t>(4, subs_.size());
    double s = 0;
    for (size_t k = subs_.size() - n; k < subs_.size(); ++k) s += subs_[k];
    return loudnessOf(s / 4.0);
}

double LoudnessMeter::shortTerm() const
{
    if (subs_.empty()) return kSilence;
    double s = 0;
    for (double v : subs_) s += v;
    return loudnessOf(s / 30.0);
}

double LoudnessMeter::integrated() const
{
    // the absolute gate, then the relative one (10 LU below the loudness of what passed the first)
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0;
    size_t n = 0;
    for (double b : blocks_)
        if (b > absGate) { sum += b; ++n; }
    if (!n) return kSilence;
    const double relGate = (sum / n) * std::pow(10.0, -10.0 / 10.0);
    double s2 = 0;
    size_t n2 = 0;
    for (double b : blocks_)
        if (b > absGate && b > relGate) { s2 += b; ++n2; }
    return n2 ? loudnessOf(s2 / n2) : kSilence;
}

double LoudnessMeter::range() const
{
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0;
    size_t n = 0;
    for (double b : shortBlocks_)
        if (b > absGate) { sum += b; ++n; }
    if (n < 2) return 0.0;
    const double relGate = (sum / n) * std::pow(10.0, -20.0 / 10.0);
    std::vector<double> l;
    for (double b : shortBlocks_)
        if (b > absGate && b > relGate) l.push_back(loudnessOf(b));
    if (l.size() < 2) return 0.0;
    std::sort(l.begin(), l.end());
    auto pct = [&](double p) {
        const double at = p * (l.size() - 1);
        const size_t i = (size_t)std::floor(at);
        const double f = at - i;
        return i + 1 < l.size() ? l[i] * (1 - f) + l[i + 1] * f : l[i];
    };
    return pct(0.95) - pct(0.10);
}

double LoudnessMeter::channelTruePeak(int ch) const
{
    const double v = ch >= 0 && ch < 6 ? chPeak_[ch] : 0;
    return v > 0 ? 20.0 * std::log10(v) : -144.0;
}

double LoudnessMeter::recentTruePeak(int ch) const
{
    const double v = ch >= 0 && ch < 6 ? std::max(lastPeak_[ch], curPeak_[ch]) : 0;
    return v > 0 ? 20.0 * std::log10(v) : -144.0;
}

double LoudnessMeter::truePeak() const
{
    return peak_ > 0 ? 20.0 * std::log10(peak_) : -144.0;
}

static std::string lufsText(double v)
{
    if (v <= LoudnessMeter::kSilence + 1e-9) return "-inf";
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

static std::string clockText(double s)
{
    if (s < 0) s = 0;
    const long long ms = (long long)std::llround(s * 1000.0);
    char b[48];
    std::snprintf(b, sizeof b, "%02lld:%02lld:%02lld.%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
    return b;
}

std::string loudnessLogText(const std::string& name, const std::string& format, bool atsc, double from, double to, const LoudnessMeter& m,
                            const std::vector<LoudnessLogPoint>& points)
{
    const double target = atsc ? -24.0 : -23.0, peakMax = atsc ? -2.0 : -1.0;
    std::string t;
    char b[512];
    t += "Loudness Log\r\n\r\n";
    t += "File: " + name + "\r\n";
    if (!format.empty()) t += "Format: " + format + "\r\n";
    t += std::string("Metering mode: ") + (atsc ? "ATSC A/85 (target -24 LUFS, maximum true peak -2.0 dBTP)" : "EBU R 128 (target -23 LUFS, maximum true peak -1.0 dBTP)") + "\r\n";
    std::snprintf(b, sizeof b, "Measured: %s to %s (%.3f seconds)\r\n\r\n", clockText(from).c_str(), clockText(to).c_str(), std::max(0.0, to - from));
    t += b;
    const double I = m.integrated(), tp = m.truePeak();
    t += "Integrated:         " + lufsText(I) + " LUFS" + (I > target + 1.0 + 1e-9 ? "   (over the target)" : I < target - 1.0 - 1e-9 && I > LoudnessMeter::kSilence ? "   (under the target)" : "") + "\r\n";
    std::snprintf(b, sizeof b, "Loudness range:     %.1f LU\r\n", m.range());
    t += b;
    t += "True peak:          " + (tp <= -143 ? std::string("-inf") : lufsText(tp)) + " dBTP" + (tp > peakMax + 1e-9 ? "   (over the maximum)" : "") + "\r\n";
    t += "Maximum momentary:  " + lufsText(m.maxMomentary()) + " LUFS\r\n";
    t += "Maximum short-term: " + lufsText(m.maxShortTerm()) + " LUFS\r\n\r\n";
    t += "Time           Momentary  Short-term (LUFS)\r\n";
    for (const LoudnessLogPoint& p : points) {
        std::snprintf(b, sizeof b, "%s  %9s  %10s\r\n", clockText(p.time).c_str(), lufsText(p.momentary).c_str(), lufsText(p.shortTerm).c_str());
        t += b;
    }
    t += "\r\nMeasured as ITU-R BS.1770-4 and EBU Tech 3341 / 3342 describe.\r\n";
    return t;
}

}  // namespace reno
