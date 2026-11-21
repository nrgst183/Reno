// stretch_samples_test: Reno's time stretching against VEGAS's, on the renders tools/vegas/Stretch Samples.cs makes.
//
//   stretch_samples_test <samples folder> <source sound file> [report.csv]
//     (the samples folder holds index.csv and VEGAS's WAVs: "<file> - stretch samples" beside the source)
//
// Each row of index.csv is made again in Reno: one audio event of the source's excerpt, the same playback rate, event
// length, method and élastique mode, rendered by the audio engine over the event's span. Against VEGAS's file it gets:
//   lag          the shift (samples, Reno later if positive) at which the two correlate best, within +-100 ms
//   corr_best    the correlation there (mono), corr_0 at no shift
//   level_db     Reno's RMS against VEGAS's
//   spec_db      the mean difference of log magnitudes over 2048-sample windows (dB), all bands; low/mid/high below 500 Hz,
//                500 Hz-4 kHz, above 4 kHz
//   env_corr     how the two loudness curves (RMS per 10 ms) follow each other: timing and transients
// The report (default stretch_report.csv in the current folder) has a line per file and the means per mode at the end.
// Exit code: 0 (it measures; it does not judge).
#include "../../src/engine/engine.h"
#include "../../src/core/base.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace reno;

namespace {

std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (c == '"') q = false;
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * 3.14159265358979323846 / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double rms(const std::vector<float>& x)
{
    double e = 0;
    for (float v : x) e += (double)v * v;
    return x.empty() ? 0 : std::sqrt(e / (double)x.size());
}

// correlation of a against b shifted by 'lag' (a[i] with b[i - lag]), over where both are
double corrAt(const std::vector<float>& a, const std::vector<float>& b, int lag)
{
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const long j = (long)i - lag;
        if (j < 0 || j >= (long)b.size()) continue;
        ab += (double)a[i] * b[(size_t)j];
        aa += (double)a[i] * a[i];
        bb += (double)b[(size_t)j] * b[(size_t)j];
    }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0;
}

// best lag by FFT cross-correlation, within +-maxLag
int bestLag(const std::vector<float>& reno, const std::vector<float>& veg, int maxLag)
{
    size_t n = 1;
    while (n < reno.size() + veg.size()) n <<= 1;
    std::vector<std::complex<double>> A(n), B(n);
    for (size_t i = 0; i < reno.size(); ++i) A[i] = reno[i];
    for (size_t i = 0; i < veg.size(); ++i) B[i] = veg[i];
    fft(A);
    fft(B);
    for (size_t i = 0; i < n; ++i) A[i] *= std::conj(B[i]);
    // inverse by conjugation
    for (auto& v : A) v = std::conj(v);
    fft(A);
    int best = 0;
    double bv = -1e300;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        const double v = A[(size_t)((lag + (long)n) % (long)n)].real();
        if (v > bv) { bv = v; best = lag; }
    }
    return best;
}

struct Spec { double all = 0, low = 0, mid = 0, high = 0; };

// mean |20 log10 |A| - 20 log10 |B|| over Hann windows where either is above -80 dB, by band
Spec specDiff(const std::vector<float>& a, const std::vector<float>& b, int rate)
{
    const size_t N = 2048, hop = 512;
    Spec s;
    double cAll = 0, cLow = 0, cMid = 0, cHigh = 0;
    const size_t len = std::min(a.size(), b.size());
    std::vector<double> win(N);
    for (size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * (double)i / (double)N);
    for (size_t at = 0; at + N <= len; at += hop) {
        std::vector<std::complex<double>> A(N), B(N);
        for (size_t i = 0; i < N; ++i) {
            A[i] = a[at + i] * win[i];
            B[i] = b[at + i] * win[i];
        }
        fft(A);
        fft(B);
        for (size_t k = 1; k < N / 2; ++k) {
            const double ma = std::abs(A[k]) / (N / 4.0), mb = std::abs(B[k]) / (N / 4.0);
            const double da = 20 * std::log10(ma + 1e-9), db = 20 * std::log10(mb + 1e-9);
            if (da < -80 && db < -80) continue;
            const double d = std::fabs(std::max(da, -80.0) - std::max(db, -80.0));
            const double hz = (double)k * rate / (double)N;
            s.all += d; cAll++;
            if (hz < 500) { s.low += d; cLow++; }
            else if (hz < 4000) { s.mid += d; cMid++; }
            else { s.high += d; cHigh++; }
        }
    }
    s.all = cAll ? s.all / cAll : 0;
    s.low = cLow ? s.low / cLow : 0;
    s.mid = cMid ? s.mid / cMid : 0;
    s.high = cHigh ? s.high / cHigh : 0;
    return s;
}

// Reno's energy against VEGAS's per octave (dB, positive: Reno louder there), summed over 4096-sample Hann windows:
// the centres 63 Hz .. 16 kHz
const int kOctaves = 9;
const double kOctaveHz[kOctaves] = {63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
std::vector<double> octaveDiff(const std::vector<float>& a, const std::vector<float>& b, int rate)
{
    const size_t N = 4096, hop = 2048;
    std::vector<double> ea(kOctaves, 0.0), eb(kOctaves, 0.0);
    const size_t len = std::min(a.size(), b.size());
    for (size_t at = 0; at + N <= len; at += hop) {
        std::vector<std::complex<double>> A(N), B(N);
        for (size_t i = 0; i < N; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * (double)i / (double)N);
            A[i] = a[at + i] * w;
            B[i] = b[at + i] * w;
        }
        fft(A);
        fft(B);
        for (size_t k = 1; k < N / 2; ++k) {
            const double hz = (double)k * rate / (double)N;
            for (int o = 0; o < kOctaves; ++o)
                if (hz >= kOctaveHz[o] / std::sqrt(2.0) && hz < kOctaveHz[o] * std::sqrt(2.0)) {
                    ea[(size_t)o] += std::norm(A[k]);
                    eb[(size_t)o] += std::norm(B[k]);
                }
        }
    }
    std::vector<double> d(kOctaves, 0.0);
    for (int o = 0; o < kOctaves; ++o) d[(size_t)o] = ea[(size_t)o] > 0 && eb[(size_t)o] > 0 ? 10 * std::log10(ea[(size_t)o] / eb[(size_t)o]) : 0.0;
    return d;
}

std::vector<double> envelope(const std::vector<float>& x, int step)
{
    std::vector<double> e;
    for (size_t at = 0; at + (size_t)step <= x.size(); at += (size_t)step) {
        double s = 0;
        for (int i = 0; i < step; ++i) s += (double)x[at + (size_t)i] * x[at + (size_t)i];
        e.push_back(std::sqrt(s / step));
    }
    return e;
}

double pearson(const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double)n;
    mb /= (double)n;
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i) {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0;
}

bool readAll(const std::string& path, int rate, int64_t frames, std::vector<float>& l, std::vector<float>& r)
{
    std::string err;
    std::unique_ptr<MediaReader> rd = openMediaOrProject(path, &err);
    if (!rd) return false;
    l.assign((size_t)frames, 0.f);
    r.assign((size_t)frames, 0.f);
    for (int64_t at = 0; at < frames; at += 65536) {
        const int n = (int)std::min<int64_t>(65536, frames - at);
        if (!rd->audio(at, n, rate, l.data() + at, r.data() + at)) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: stretch_samples_test <samples folder> <source sound file> [report.csv]\n");
        return 2;
    }
    const std::string dir = argv[1], source = argv[2], reportPath = argc > 3 ? argv[3] : "stretch_report.csv";
    std::ifstream idx(pathJoin(dir, "index.csv"));
    if (!idx) {
        std::printf("no index.csv in %s\n", dir.c_str());
        return 2;
    }
    std::string line;
    std::getline(idx, line);
    const std::vector<std::string> head = splitCsv(line);
    auto col = [&](const char* name) { return (int)(std::find(head.begin(), head.end(), std::string(name)) - head.begin()); };
    const int cFile = col("file"), cMethod = col("method"), cMode = col("mode"), cRate = col("playback_rate"), cLen = col("event_length_s"),
              cStart = col("excerpt_start_s"), cSr = col("sample_rate");

    std::unique_ptr<MediaReader> probe = openMediaOrProject(source, nullptr);
    if (!probe) {
        std::printf("cannot open %s\n", source.c_str());
        return 2;
    }
    const MediaInfo srcInfo = probe->info();
    probe.reset();

    std::ofstream rep(reportPath);
    rep << "file,mode,rate,samples_vegas,samples_reno,lag,corr_0,corr_best,level_db,spec_db,spec_low_db,spec_mid_db,spec_high_db,env_corr";
    for (int o = 0; o < kOctaves; ++o) rep << ",oct_" << (int)kOctaveHz[o] << "_db";
    rep << "\n";
    struct Sum { int n = 0; double corr = 0, level = 0, spec = 0, low = 0, mid = 0, high = 0, env = 0, absLag = 0; };
    std::map<std::string, Sum> sums;

    MediaPool pool;
    FxRuntime fx;
    while (std::getline(idx, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = splitCsv(line);
        if ((int)f.size() <= std::max({cFile, cMethod, cMode, cRate, cLen, cStart, cSr})) continue;
        const std::string file = f[(size_t)cFile], method = toLower(f[(size_t)cMethod]), mode = toLower(f[(size_t)cMode]);
        const double rate = std::stod(f[(size_t)cRate]), len = std::stod(f[(size_t)cLen]), start = std::stod(f[(size_t)cStart]);
        const int sr = std::stoi(f[(size_t)cSr]);

        Project p;
        p.settings.sampleRate = sr;
        MediaItem m;
        m.id = p.newId();
        m.path = source;
        m.name = pathFileName(source);
        m.info = srcInfo;
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.start = 0;
        e.length = len;
        e.offset = start;
        e.rate = rate;
        e.loop = true;
        e.stretchMethod = method == "none" ? "none" : "elastique";
        if (!mode.empty()) e.stretchMode = mode;
        e.keepPitch = method != "none";        // (VEGAS: pitch not locked to the rate)
        t.events.push_back(e);
        p.tracks.push_back(t);

        // VEGAS's file
        std::vector<float> vl, vr;
        std::unique_ptr<MediaReader> vrd = openMediaOrProject(pathJoin(dir, file), nullptr);
        if (!vrd) { std::printf("SKIP %s: cannot read it\n", file.c_str()); continue; }
        const int64_t vframes = (int64_t)std::llround(vrd->info().duration * sr);
        vrd.reset();
        if (!readAll(pathJoin(dir, file), sr, vframes, vl, vr)) { std::printf("SKIP %s: cannot read it\n", file.c_str()); continue; }

        // Reno's
        const int64_t frames = (int64_t)std::floor(len * sr + 1e-6);
        std::vector<float> rl((size_t)frames), rr((size_t)frames);
        {
            AudioEngine eng(pool, fx);
            FinalRenderScope final;
            for (int64_t at = 0; at < frames; at += 4096) {
                const int n = (int)std::min<int64_t>(4096, frames - at);
                eng.render(p, at, n, rl.data() + at, rr.data() + at, true);
            }
        }

        std::vector<float> vm(vl.size()), rm(rl.size());
        for (size_t i = 0; i < vl.size(); ++i) vm[i] = 0.5f * (vl[i] + vr[i]);
        for (size_t i = 0; i < rl.size(); ++i) rm[i] = 0.5f * (rl[i] + rr[i]);
        const int lag = bestLag(rm, vm, sr / 10);
        const double c0 = corrAt(rm, vm, 0), cb = corrAt(rm, vm, lag);
        const double vrms = rms(vm), rrms = rms(rm);
        const double level = (vrms > 0 && rrms > 0) ? 20 * std::log10(rrms / vrms) : 0;
        // spectra and loudness curves with Reno moved by the lag
        std::vector<float> rshift(vm.size(), 0.f);
        for (size_t i = 0; i < vm.size(); ++i) {
            const long j = (long)i + lag;
            if (j >= 0 && j < (long)rm.size()) rshift[i] = rm[(size_t)j];
        }
        const Spec sp = specDiff(rshift, vm, sr);
        const double env = pearson(envelope(rshift, sr / 100), envelope(vm, sr / 100));

        rep << file << "," << (mode.empty() ? "none" : mode) << "," << rate << "," << vm.size() << "," << rm.size() << "," << lag << ","
            << strfmt("%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f", c0, cb, level, sp.all, sp.low, sp.mid, sp.high, env);
        for (double v : octaveDiff(rshift, vm, sr)) rep << strfmt(",%.2f", v);
        rep << "\n";
        std::printf("%-28s lag %6d  corr %.3f (at 0 %.3f)  level %+6.2f dB  spectrum %5.2f dB (low %5.2f mid %5.2f high %5.2f)  envelope %.3f\n",
                    file.c_str(), lag, cb, c0, level, sp.all, sp.low, sp.mid, sp.high, env);
        std::fflush(stdout);
        Sum& s = sums[mode.empty() ? "none" : mode];
        s.n++;
        s.corr += cb;
        s.level += std::fabs(level);
        s.spec += sp.all;
        s.low += sp.low;
        s.mid += sp.mid;
        s.high += sp.high;
        s.env += env;
        s.absLag += std::abs(lag);
    }
    rep << "\nmode,files,mean_corr_best,mean_abs_level_db,mean_spec_db,mean_spec_low,mean_spec_mid,mean_spec_high,mean_env_corr,mean_abs_lag\n";
    std::printf("\n");
    for (const auto& kv : sums) {
        const Sum& s = kv.second;
        const double n = std::max(1, s.n);
        rep << kv.first << "," << s.n << "," << strfmt("%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.1f", s.corr / n, s.level / n, s.spec / n, s.low / n, s.mid / n,
                                                         s.high / n, s.env / n, s.absLag / n) << "\n";
        std::printf("%-10s %3d files: corr %.3f  |level| %.2f dB  spectrum %.2f dB (low %.2f mid %.2f high %.2f)  envelope %.3f  |lag| %.0f\n",
                    kv.first.c_str(), s.n, s.corr / n, s.level / n, s.spec / n, s.low / n, s.mid / n, s.high / n, s.env / n, s.absLag / n);
    }
    std::printf("report: %s\n", reportPath.c_str());
    return 0;
}
