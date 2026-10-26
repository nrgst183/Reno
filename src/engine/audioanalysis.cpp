// See audioanalysis.h.
#include "audioanalysis.h"
#include <algorithm>
#include <cmath>

namespace reno {

std::vector<float> lowBandLevels(const std::vector<float>& mono, int sampleRate, double hop)
{
    std::vector<float> out;
    if (mono.empty() || sampleRate <= 0 || !(hop > 0)) return out;
    const int h = std::max(1, (int)std::lround(hop * sampleRate));
    // two one-pole low-pass filters in a row (12 dB an octave) at 1.5 kHz
    const double a = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * 1500.0 / sampleRate);
    double s1 = 0, s2 = 0;
    out.reserve(mono.size() / (size_t)h + 1);
    double acc = 0;
    int n = 0;
    for (size_t i = 0; i < mono.size(); ++i) {
        const double x = std::isfinite(mono[i]) ? mono[i] : 0.0;
        s1 += a * (x - s1);
        s2 += a * (s1 - s2);
        acc += s2 * s2;
        if (++n == h) {
            const double rms = std::sqrt(acc / h);
            out.push_back((float)(20.0 * std::log10(std::max(1e-6, rms))));
            acc = 0;
            n = 0;
        }
    }
    return out;
}

namespace {

// How much louder each step is than the one before (dB, rises only), lightly smoothed.
std::vector<float> flux(const std::vector<float>& level)
{
    std::vector<float> f(level.size(), 0.f);
    for (size_t i = 1; i < level.size(); ++i) f[i] = std::max(0.f, level[i] - level[i - 1]);
    std::vector<float> g(f.size(), 0.f);
    for (size_t i = 0; i < f.size(); ++i) g[i] = f[i] + (i + 1 < f.size() ? 0.5f * f[i + 1] : 0.f);
    return g;
}

}  // namespace

std::vector<double> detectBeats(const std::vector<float>& mono, int sampleRate, double sensitivity)
{
    std::vector<double> out;
    const double hop = 0.01;
    const std::vector<float> level = lowBandLevels(mono, sampleRate, hop);
    if (level.size() < 8) return out;
    const std::vector<float> f = flux(level);
    const int n = (int)f.size();
    sensitivity = std::min(1.0, std::max(0.0, sensitivity));
    const double k = 0.3 + 3.2 * sensitivity;           // how many standard deviations above the local mean a hit must be
    const int win = (int)std::lround(1.0 / hop);          // the local mean and spread: a second either way
    // running sums for the window
    std::vector<double> sum(n + 1, 0.0), sum2(n + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        sum[i + 1] = sum[i] + f[i];
        sum2[i + 1] = sum2[i] + (double)f[i] * f[i];
    }
    const int minGap = (int)std::lround(0.12 / hop);
    int last = -minGap;
    for (int i = 1; i + 1 < n; ++i) {
        if (!(f[i] >= f[i - 1] && f[i] > f[i + 1])) continue;
        const int a = std::max(0, i - win), b = std::min(n, i + win + 1);
        const double cnt = b - a;
        const double mean = (sum[b] - sum[a]) / cnt;
        const double var = std::max(0.0, (sum2[b] - sum2[a]) / cnt - mean * mean);
        const double thr = mean + k * std::sqrt(var) + 1.0 + 4.0 * sensitivity;
        if (f[i] <= thr) continue;
        if (i - last < minGap) {
            if (!out.empty() && f[i] > f[last]) {
                out.back() = i * hop;
                last = i;
            }
            continue;
        }
        out.push_back(i * hop);
        last = i;
    }
    return out;
}

double detectTempo(const std::vector<float>& mono, int sampleRate, double* confidence)
{
    if (confidence) *confidence = 0;
    const double hop = 0.01;
    std::vector<float> part = mono;
    const size_t most = (size_t)sampleRate * 120;
    if (part.size() > most) {
        const size_t from = (part.size() - most) / 2;
        part = std::vector<float>(mono.begin() + (std::ptrdiff_t)from, mono.begin() + (std::ptrdiff_t)(from + most));
    }
    const std::vector<float> level = lowBandLevels(part, sampleRate, hop);
    if (level.size() < 400) return 0;
    std::vector<float> f = flux(level);
    const int n = (int)f.size();
    // take the mean out
    double mean = 0;
    for (float v : f) mean += v;
    mean /= n;
    for (float& v : f) v = (float)(v - mean);
    double energy = 0;
    for (float v : f) energy += (double)v * v;
    if (!(energy > 1e-9)) return 0;
    auto ac = [&](double lag) {
        // the flux against itself 'lag' steps later (between steps: interpolated)
        const int l0 = (int)std::floor(lag);
        const double fr = lag - l0;
        double s = 0;
        for (int i = 0; i + l0 + 1 < n; ++i) s += (double)f[i] * (f[i + l0] * (1.0 - fr) + f[i + l0 + 1] * fr);
        return s / energy;
    };
    double bestScore = -1e9, bestBpm = 0, total = 0;
    int count = 0;
    std::vector<std::pair<double, double>> scores;
    for (double bpm = 60.0; bpm <= 200.0; bpm += 0.25) {
        const double period = 60.0 / bpm / hop;                // in steps
        double s = 0;
        for (int m = 1; m <= 4; ++m) s += ac(period * m) / m;
        // a listener's preference: about 120 beats a minute, falling off by octaves
        const double oct = std::log2(bpm / 120.0);
        s *= std::exp(-0.5 * oct * oct / (0.85 * 0.85));
        scores.push_back({bpm, s});
        total += std::max(0.0, s);
        ++count;
        if (s > bestScore) {
            bestScore = s;
            bestBpm = bpm;
        }
    }
    if (!(bestScore > 0)) return 0;
    // refine around the best: a finer step
    double fineBest = bestScore, fineBpm = bestBpm;
    for (double bpm = bestBpm - 0.5; bpm <= bestBpm + 0.5; bpm += 0.02) {
        const double period = 60.0 / bpm / hop;
        double s = 0;
        for (int m = 1; m <= 4; ++m) s += ac(period * m) / m;
        const double oct = std::log2(bpm / 120.0);
        s *= std::exp(-0.5 * oct * oct / (0.85 * 0.85));
        if (s > fineBest) {
            fineBest = s;
            fineBpm = bpm;
        }
    }
    if (confidence) {
        const double avg = count ? total / count : 0;
        *confidence = std::min(1.0, std::max(0.0, avg > 0 ? (fineBest / avg - 1.0) / 6.0 : 0.0));
    }
    return std::round(fineBpm * 100.0) / 100.0;
}

}  // namespace reno
