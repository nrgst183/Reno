// Time-stretchers for events' sound (see stretch.h).
#include "stretch.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace reno {

namespace {

const double kPi = 3.14159265358979323846;
using cplx = std::complex<double>;

// ------------------------------------------------------------------------------------------ input
// The stretch of input a stretcher is looking at: [base, end).
class InputWindow {
public:
    void need(int64_t lo, int64_t hi, const StretchPull& pull)
    {
        if (hi < lo) hi = lo;
        if (!have_ || lo < base_ || lo > end_) {
            have_ = true;
            base_ = end_ = lo;
            off_ = 0;
            l_.clear();
            r_.clear();
        }
        // forget what comes before lo (compacting now and then)
        off_ += (size_t)(lo - base_);
        base_ = lo;
        if (off_ > 65536 && off_ * 2 > l_.size()) {
            l_.erase(l_.begin(), l_.begin() + (std::ptrdiff_t)off_);
            r_.erase(r_.begin(), r_.begin() + (std::ptrdiff_t)off_);
            off_ = 0;
        }
        while (end_ < hi) {
            const int k = (int)std::min<int64_t>(std::max<int64_t>(hi - end_, kFetch), 1 << 20);
            const size_t at = l_.size();
            l_.resize(at + (size_t)k);
            r_.resize(at + (size_t)k);
            pull(end_, k, l_.data() + at, r_.data() + at);
            end_ += k;
        }
    }
    const float* l(int64_t i) const { return l_.data() + off_ + (size_t)(i - base_); }
    const float* r(int64_t i) const { return r_.data() + off_ + (size_t)(i - base_); }
    int64_t end() const { return end_; }

private:
    static constexpr int kFetch = 2048;
    std::vector<float> l_, r_;
    size_t off_ = 0;
    int64_t base_ = 0, end_ = 0;
    bool have_ = false;
};

// ------------------------------------------------------------------------------------------ FFT
// An in-place radix-2 FFT (n a power of 2). Unscaled both ways.
class Fft {
public:
    explicit Fft(int n) : n_(n), t_(tablesFor(n)) {}
    void forward(cplx* x) const { run(x, t_->tw.data()); }
    void inverse(cplx* x) const { run(x, t_->twc.data()); }
    int size() const { return n_; }

private:
    // The bit-reversal order and the twiddle factors.
    struct Tables {
        std::vector<int> rev;
        std::vector<cplx> tw, twc;
    };
    static std::shared_ptr<const Tables> tablesFor(int n)
    {
        static std::mutex mutex;
        static std::map<int, std::shared_ptr<const Tables>> made;
        std::lock_guard<std::mutex> lk(mutex);
        std::shared_ptr<const Tables>& t = made[n];
        if (!t) {
            std::shared_ptr<Tables> m = std::make_shared<Tables>();
            m->rev.resize((size_t)n);
            m->tw.resize((size_t)(n / 2));
            m->twc.resize((size_t)(n / 2));
            int bits = 0;
            while ((1 << bits) < n) ++bits;
            for (int i = 0; i < n; ++i) {
                int r = 0;
                for (int b = 0; b < bits; ++b)
                    if (i & (1 << b)) r |= 1 << (bits - 1 - b);
                m->rev[(size_t)i] = r;
            }
            for (int i = 0; i < n / 2; ++i) {
                m->tw[(size_t)i] = std::polar(1.0, -2.0 * kPi * i / n);
                m->twc[(size_t)i] = std::conj(m->tw[(size_t)i]);
            }
            t = m;
        }
        return t;
    }

    void run(cplx* x, const cplx* tw) const
    {
        const int* rev = t_->rev.data();
        for (int i = 0; i < n_; ++i)
            if (i < rev[i]) std::swap(x[i], x[rev[i]]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len >> 1, step = n_ / len;
            for (int i = 0; i < n_; i += len) {
                for (int j = 0; j < half; ++j) {
                    const double wr = tw[j * step].real(), wi = tw[j * step].imag();
                    const cplx a = x[i + j];
                    const double xr = x[i + j + half].real(), xi = x[i + j + half].imag();
                    const double br = xr * wr - xi * wi, bi = xr * wi + xi * wr;
                    x[i + j] = cplx(a.real() + br, a.imag() + bi);
                    x[i + j + half] = cplx(a.real() - br, a.imag() - bi);
                }
            }
        }
    }
    int n_;
    std::shared_ptr<const Tables> t_;
};

// The Hann window of n samples, worked out once per size (see Fft)
std::vector<double> hannWindow(int n)
{
    static std::mutex mutex;
    static std::map<int, std::vector<double>> made;
    std::lock_guard<std::mutex> lk(mutex);
    std::vector<double>& w = made[n];
    if (w.empty()) {
        w.resize((size_t)n);
        for (int k = 0; k < n; ++k) w[(size_t)k] = 0.5 - 0.5 * std::cos(2.0 * kPi * k / n);
    }
    return w;
}

int pow2Near(double v)
{
    int n = 64;
    while (n * 1.5 < v) n <<= 1;
    return n;
}

// Hands out what is ready (a run of finished output samples) and asks for more when it runs out.
struct ReadyBuffer {
    std::vector<float> l, r;
    int at = 0, n = 0;
    // copies what is ready into out; returns how many
    int take(float* ol, float* or_, int want)
    {
        const int k = std::min(want, n - at);
        if (k <= 0) return 0;
        std::memcpy(ol, l.data() + at, (size_t)k * sizeof(float));
        std::memcpy(or_, r.data() + at, (size_t)k * sizeof(float));
        at += k;
        return k;
    }
};

// ------------------------------------------------------------------------------------------ Classic
// VEGAS's Classic stretcher cuts the input into splices.
const std::vector<ClassicPreset> kClassic = {
    // VEGAS's own numbers (the table its Classic stretcher uses): half splice, search, step, crossfade
    {"A01", "A01. Music 1 (minimum flange, may echo)", 300, 15, 2, 15},
    {"A02", "A02. Music 2", 275, 22, 2, 20},
    {"A03", "A03. Music 3 (less echo)", 150, 15, 2, 15},
    {"A04", "A04. Music 4 (fast, good for bass)", 220, 15, 4, 15},
    {"A05", "A05. Music 5", 190, 15, 2, 15},
    {"A06", "A06. Music 6", 150, 15, 3, 10},
    {"A07", "A07. Speech 1", 90, 15, 2, 30},
    {"A08", "A08. Speech 2", 78, 12, 2, 10},
    {"A09", "A09. Speech 3 (fast)", 80, 12, 4, 10},
    {"A10", "A10. Solo instruments 1", 150, 25, 2, 30},
    {"A11", "A11. Solo instruments 2", 130, 25, 3, 30},
    {"A12", "A12. Solo instruments 3", 110, 25, 2, 30},
    {"A13", "A13. Solo instruments 4 (less echo)", 75, 10, 2, 10},
    {"A14", "A14. Solo instruments 5", 90, 15, 2, 12},
    {"A15", "A15. Solo instruments 6", 85, 15, 4, 12},
    {"A16", "A16. Solo instruments 7 (fast)", 95, 15, 8, 12},
    {"A17", "A17. Drums, unpitched (minimum echo)", 50, 5, 2, 3},
    {"A18", "A18. Drums (better for toms)", 70, 9, 2, 8},
    {"A19", "A19. Drums (tiny echo)", 50, 7, 2, 8},
};

class ClassicStretcher : public TimeStretcher {
public:
    ClassicStretcher(const ClassicPreset& pr, int rate, double alpha, int64_t inputEnd) : alpha_(alpha), inputEnd_(inputEnd)
    {
        // Measured on VEGAS Pro 18's own renders of Classic A03 events.
        H_ = std::max(32, (int)std::lround(0.5 * pr.halfSpliceMs * rate / 1000.0));
        C_ = std::max(8, std::min((int)std::lround(pr.crossfadeMs * rate / 1000.0), H_ / 2));
        L_ = H_ + C_;
                S_ = std::max(1, (int)std::lround(pr.searchMs * rate / 1000.0));
        step_ = std::max(1, (int)std::lround(pr.searchStep * rate / 44100.0));
        win_.resize((size_t)L_);
        for (int j = 0; j < L_; ++j) {
            double w = 1.0;
            if (j >= L_ - C_) w = (L_ - j - 0.5) / C_;
            win_[(size_t)j] = (float)w;
        }
        accL_.assign((size_t)L_, 0.f);
        accR_.assign((size_t)L_, 0.f);
        out_.l.assign((size_t)H_, 0.f);
        out_.r.assign((size_t)H_, 0.f);
    }

    void process(float* l, float* r, int n, const StretchPull& pull) override
    {
        int done = 0;
        while (done < n) {
            const int k = out_.take(l + done, r + done, n - done);
            if (k > 0) done += k;
            else splice(pull);
        }
    }

private:
    // how well the C samples at b continue the C samples at a (normalised correlation of the mid signal)
    double score(int64_t a, int64_t b) const
    {
        const float *al = in_.l(a), *ar = in_.r(a), *bl = in_.l(b), *br = in_.r(b);
        double dot = 0, eb = 0;
        for (int j = 0; j < C_; ++j) {
            const double x = (double)al[j] + ar[j], y = (double)bl[j] + br[j];
            dot += x * y;
            eb += y * y;
        }
        return dot / std::sqrt(eb + 1e-9);
    }

    void splice(const StretchPull& pull)
    {
        // Splice i is laid at [i H - C, i H - C + L) of the output and is at full strength from i H on.
        const int64_t nom = (int64_t)std::llround((double)(i_ * H_) / alpha_);
        int64_t lo = nom - S_, hi = nom + S_ + L_;
        const int64_t natural = prevPos_ + H_;
        if (havePrev_) {
            lo = std::min(lo, natural);
            hi = std::max(hi, natural + L_);
        }
        in_.need(lo, hi, pull);
        int64_t pos = nom;
        const bool fits = inputEnd_ < 0 || nom + S_ + L_ <= inputEnd_;
        if (havePrev_ && fits) {
            // the continuation has to have something in it to line up with
            double e = 0;
            const float *xl = in_.l(natural), *xr = in_.r(natural);
            for (int j = 0; j < C_; ++j) e += ((double)xl[j] + xr[j]) * ((double)xl[j] + xr[j]);
            if (e > 1e-9) {
                double best = -1e300;
                for (int64_t c = nom - S_; c <= nom + S_; c += step_) {
                    const double s = score(natural, c);
                    if (s > best) { best = s; pos = c; }
                }
                const int64_t centre = pos;
                for (int64_t c = std::max(nom - S_, centre - step_ + 1); c <= std::min(nom + S_, centre + step_ - 1); ++c) {
                    const double s = score(natural, c);
                    if (s > best) { best = s; pos = c; }
                }
            }
        }
        const float *sl = in_.l(pos), *sr = in_.r(pos);
        for (int j = 0; j < L_; ++j) {
            const float w = (i_ > 0 && j < C_) ? (float)((j + 0.5) / C_) : win_[(size_t)j];
            accL_[(size_t)j] += w * sl[j];
            accR_[(size_t)j] += w * sr[j];
        }
        // [i H, i H + H) of the output is finished
        out_.at = 0;
        out_.n = H_;
        std::memcpy(out_.l.data(), accL_.data(), (size_t)H_ * sizeof(float));
        std::memcpy(out_.r.data(), accR_.data(), (size_t)H_ * sizeof(float));
        std::memmove(accL_.data(), accL_.data() + H_, (size_t)(L_ - H_) * sizeof(float));
        std::memmove(accR_.data(), accR_.data() + H_, (size_t)(L_ - H_) * sizeof(float));
        std::fill(accL_.begin() + (L_ - H_), accL_.end(), 0.f);
        std::fill(accR_.begin() + (L_ - H_), accR_.end(), 0.f);
        prevPos_ = pos;
        havePrev_ = true;
        ++i_;
    }

    double alpha_;
    int64_t inputEnd_ = -1;
    int L_ = 0, C_ = 0, H_ = 0, S_ = 0, step_ = 1;
    std::vector<float> win_, accL_, accR_;
    ReadyBuffer out_;
    InputWindow in_;
    int64_t i_ = 0, prevPos_ = 0;
    bool havePrev_ = false;
};

// ------------------------------------------------------------------------------------------ phase vocoder
// élastique Pro / Efficient stand-in.
class PhaseVocoder : public TimeStretcher {
public:
    PhaseVocoder(const StretchParams& p, bool pro) : alpha_(p.alpha), pro_(pro), fft_(pow2Near(p.rate * (pro ? 0.085 : 0.043)))
    {
        N_ = fft_.size();
        Hs_ = N_ / (pro ? 8 : 4);
        const int nb = N_ / 2 + 1;
        win_ = hannWindow(N_);
        norm_ = 1.0 / (0.375 * N_ / Hs_);
        z_.resize((size_t)N_);
        cep_.resize((size_t)N_);
        mag_.assign((size_t)nb, 0.0);
        phi_.assign((size_t)nb, 0.0);
        prevPhi_.assign((size_t)nb, 0.0);
        theta_.assign((size_t)nb, 0.0);
        prevMag_.assign((size_t)nb, 0.0);
        adv_.assign((size_t)nb, 0.0);
        gain_.assign((size_t)nb, 1.0);
        env_.assign((size_t)nb, 0.0);
        xl_.resize((size_t)nb);
        xr_.resize((size_t)nb);
        accL_.assign((size_t)N_, 0.0);
        accR_.assign((size_t)N_, 0.0);
        out_.l.assign((size_t)Hs_, 0.f);
        out_.r.assign((size_t)Hs_, 0.f);
        // formants: the envelope of the input (pitched by 'resampled') is read at bin k x ratio
        if (pro && p.preserveFormants) {
            formantRatio_ = p.resampled / std::pow(2.0, p.formant / 12.0);
            formants_ = std::fabs(formantRatio_ - 1.0) > 1e-6;
            lifter_ = std::max(8, (int)std::lround(p.rate * 0.0012));
        }
    }

    void process(float* l, float* r, int n, const StretchPull& pull) override
    {
        int done = 0;
        while (done < n) {
            const int k = out_.take(l + done, r + done, n - done);
            if (k > 0) done += k;
            else frame(pull);
        }
    }

private:
    static double wrap(double a) { return a - 2.0 * kPi * std::floor((a + kPi) / (2.0 * kPi)); }

    // Gain correction: élastique's output level is below what an energy-keeping vocoder gives once the sound is
    // stretched.
    double levelGain(double energy)
    {
        // static const: MSVC does not let a lambda or a function-local use a block constant otherwise; ln(alpha) against level.
        static const double kX[] = {-1.3863, -1.1087, -0.6931, -0.4005, -0.2231, -0.1054, 0.0583, 0.1740, 0.4055, 0.6931};
        static const double kEff[] = {0.68, 0.71, 0.77, 0.82, 0.89, 0.90, 0.94, 0.94, 0.99, 1.0};
        static const double kPro[] = {1.0, 1.0, 1.0, 0.97, 0.94, 0.90, 0.90, 0.91, 0.95, 1.0};
        const double* tab = pro_ ? kPro : kEff;
        const double x = std::log(std::max(1e-3, alpha_));
        double G;
        if (x <= kX[0]) G = tab[0];
        else if (x >= kX[9]) G = tab[9];
        else {
            int i = 0;
            while (x > kX[i + 1]) ++i;
            G = tab[i] + (tab[i + 1] - tab[i]) * (x - kX[i]) / (kX[i + 1] - kX[i]);
        }
        const double w = std::min(1.0, noisiness(energy) / 0.01);
        return (pro_ ? 0.989 : 1.0) * (1.0 - (1.0 - G) * w);
    }

    // the log spectral envelope of mag_ into env_ (real cepstrum, low quefrencies kept)
    void envelope()
    {
        const int nb = N_ / 2 + 1;
        for (int k = 0; k < nb; ++k) {
            const double v = std::log(mag_[(size_t)k] + 1e-9);
            cep_[(size_t)k] = v;
            if (k > 0 && k < N_ / 2) cep_[(size_t)(N_ - k)] = v;
        }
        fft_.inverse(cep_.data());
        for (int q = 0; q < N_; ++q) {
            const int d = std::min(q, N_ - q);
            if (d >= lifter_) cep_[(size_t)q] = 0.0;
            else cep_[(size_t)q] /= N_;
        }
        fft_.forward(cep_.data());
        for (int k = 0; k < nb; ++k) env_[(size_t)k] = cep_[(size_t)k].real();
    }

    void frame(const StretchPull& pull)
    {
        const int nb = N_ / 2 + 1;
        const int64_t o = m_ * Hs_ - N_;                    // where this frame goes in the output
        const int64_t a = (int64_t)std::llround((o + N_ * 0.5) / alpha_ - N_ * 0.5);
        in_.need(a, a + N_, pull);
        const float *il = in_.l(a), *ir = in_.r(a);
        for (int k = 0; k < N_; ++k) z_[(size_t)k] = cplx(il[k] * win_[(size_t)k], ir[k] * win_[(size_t)k]);
        fft_.forward(z_.data());
        // the two channels' spectra from the one transform, and their sum's magnitude and phase
        double energy = 0;
        for (int k = 0; k < nb; ++k) {
            const cplx zk = z_[(size_t)k], zn = std::conj(z_[(size_t)((N_ - k) % N_)]);
            xl_[(size_t)k] = 0.5 * (zk + zn);
            xr_[(size_t)k] = cplx(0, -0.5) * (zk - zn);
            const cplx mid = xl_[(size_t)k] + xr_[(size_t)k];
            mag_[(size_t)k] = std::abs(mid);
            phi_[(size_t)k] = std::arg(mid);
            energy += mag_[(size_t)k] * mag_[(size_t)k];
        }
        frameGain_ = levelGain(energy);
        // a sharp onset: much of the spectrum got much louder at once
        bool reset = !havePrev_;
        if (pro_ && havePrev_ && energy > 1e-6) {
            int rose = 0, counted = 0;
            for (int k = 2; k < nb; ++k) {
                if (mag_[(size_t)k] < 1e-4 && prevMag_[(size_t)k] < 1e-4) continue;
                ++counted;
                if (mag_[(size_t)k] > 2.0 * prevMag_[(size_t)k]) ++rose;
            }
            if (counted > 16 && rose > counted * 0.4 && m_ - lastReset_ > 4 && energy > 2.0 * prevEnergy_) reset = true;
        }
        if (reset) {
            for (int k = 0; k < nb; ++k) theta_[(size_t)k] = phi_[(size_t)k];
            lastReset_ = m_;
        } else {
            // each bin's own frequency from how its phase moved since the last frame
            const double dA = (double)(a - prevA_);
            for (int k = 0; k < nb; ++k) {
                const double w = 2.0 * kPi * k / N_;
                double f = w;
                if (dA > 0.5) f = w + wrap(phi_[(size_t)k] - prevPhi_[(size_t)k] - w * dA) / dA;
                adv_[(size_t)k] = f * Hs_;
            }
            // peaks, and each bin's peak (the nearest one, split at the lowest point between)
            int np = 0;
            std::vector<int>& pk = peaks_;
            pk.clear();
            for (int k = 0; k < nb; ++k) {
                const double v = mag_[(size_t)k];
                bool p = true;
                for (int d = -2; d <= 2 && p; ++d) {
                    if (d == 0) continue;
                    const int j = k + d;
                    if (j < 0 || j >= nb) continue;
                    if (d < 0 ? mag_[(size_t)j] >= v : mag_[(size_t)j] > v) p = false;
                }
                if (p) pk.push_back(k);
            }
            np = (int)pk.size();
            if (np == 0) {
                for (int k = 0; k < nb; ++k) theta_[(size_t)k] += adv_[(size_t)k];
            } else {
                int from = 0;
                for (int i = 0; i < np; ++i) {
                    int to = nb;
                    if (i + 1 < np) {
                        // the lowest bin between this peak and the next
                        int lowest = pk[(size_t)i];
                        for (int k = pk[(size_t)i]; k <= pk[(size_t)i + 1]; ++k)
                            if (mag_[(size_t)k] < mag_[(size_t)lowest]) lowest = k;
                        to = lowest + 1;
                    }
                    const int p = pk[(size_t)i];
                    const double tp = theta_[(size_t)p] + adv_[(size_t)p];
                    for (int k = from; k < to; ++k)
                        if (k != p) newTheta(k, tp + phi_[(size_t)k] - phi_[(size_t)p]);
                    newTheta(p, tp);
                    from = to;
                }
                for (int k = 0; k < nb; ++k) theta_[(size_t)k] = nextTheta(k);
            }
        }
        // formants back where they were (and on by the shift)
        if (formants_) {
            envelope();
            for (int k = 0; k < nb; ++k) {
                const double x = k * formantRatio_;
                double e;
                if (x >= nb - 1) e = env_[(size_t)(nb - 1)] - 3.0;
                else {
                    const int i = (int)x;
                    const double f = x - i;
                    e = env_[(size_t)i] * (1 - f) + env_[(size_t)i + 1] * f;
                }
                gain_[(size_t)k] = std::exp(std::max(-3.0, std::min(3.0, e - env_[(size_t)k])));
            }
        }
        // both channels: their own magnitude and phase difference to the sum, turned by the sum's new phase
        for (int k = 0; k < nb; ++k) {
            const cplx turn = std::polar(formants_ ? gain_[(size_t)k] : 1.0, theta_[(size_t)k] - phi_[(size_t)k]);
            xl_[(size_t)k] *= turn;
            xr_[(size_t)k] *= turn;
        }
        for (int k = 0; k < nb; ++k) {
            const cplx yl = xl_[(size_t)k], yr = xr_[(size_t)k];
            z_[(size_t)k] = yl + cplx(0, 1) * yr;
            if (k > 0 && k < N_ / 2) z_[(size_t)(N_ - k)] = std::conj(yl) + cplx(0, 1) * std::conj(yr);
        }
        fft_.inverse(z_.data());
        const double g = norm_ / N_ * frameGain_;
        for (int k = 0; k < N_; ++k) {
            accL_[(size_t)k] += z_[(size_t)k].real() * win_[(size_t)k] * g;
            accR_[(size_t)k] += z_[(size_t)k].imag() * win_[(size_t)k] * g;
        }
        // [o, o + Hs) is finished
        out_.at = 0;
        out_.n = 0;
        if (o + Hs_ > 0) {
            const int skip = (int)std::max<int64_t>(0, -o);
            out_.n = Hs_ - skip;
            for (int k = 0; k < out_.n; ++k) {
                out_.l[(size_t)k] = (float)accL_[(size_t)(skip + k)];
                out_.r[(size_t)k] = (float)accR_[(size_t)(skip + k)];
            }
        }
        std::memmove(accL_.data(), accL_.data() + Hs_, (size_t)(N_ - Hs_) * sizeof(double));
        std::memmove(accR_.data(), accR_.data() + Hs_, (size_t)(N_ - Hs_) * sizeof(double));
        std::fill(accL_.begin() + (N_ - Hs_), accL_.end(), 0.0);
        std::fill(accR_.begin() + (N_ - Hs_), accR_.end(), 0.0);
        prevPhi_.swap(phi_);
        prevMag_.swap(mag_);
        prevEnergy_ = energy;
        prevA_ = a;
        havePrev_ = true;
        ++m_;
    }

    // How much of the frame's energy is not in tonal peaks.
    double noisiness(double energy) const
    {
        if (energy <= 1e-12) return 0;
        const int nb = N_ / 2 + 1;
        double top = 0;
        for (int k = 0; k < nb; ++k) top = std::max(top, mag_[(size_t)k]);
        const double floorMag = top * 0.05;
        double tonal = 0;
        int last = -1;
        for (int k = 0; k < nb; ++k) {
            const double v = mag_[(size_t)k];
            if (v < floorMag) continue;
            bool peak = true;
            for (int d = -2; d <= 2 && peak; ++d) {
                const int j = k + d;
                if (d != 0 && j >= 0 && j < nb && mag_[(size_t)j] > v) peak = false;
            }
            if (!peak) continue;
            const int hi = std::min(k + 2, nb - 1);
            for (int j = std::max(k - 2, last + 1); j <= hi; ++j) tonal += mag_[(size_t)j] * mag_[(size_t)j];
            last = std::max(last, hi);
        }
        return std::min(1.0, std::max(0.0, 1.0 - tonal / energy));
    }

    // new phases are gathered first: a bin's new phase must not be seen as its old one while others are worked out.
    void newTheta(int k, double v) { pending_.resize(theta_.size()); pending_[(size_t)k] = v; }
    double nextTheta(int k) const { return pending_[(size_t)k]; }

    double alpha_;
    bool pro_;
    Fft fft_;
    int N_ = 0, Hs_ = 0;
    double norm_ = 1;
    std::vector<double> win_, mag_, phi_, prevPhi_, theta_, prevMag_, adv_, gain_, env_, pending_, accL_, accR_;
    std::vector<int> peaks_;
    std::vector<cplx> z_, cep_, xl_, xr_;
    double prevEnergy_ = 0;
    double frameGain_ = 1;
    bool formants_ = false;
    double formantRatio_ = 1;
    int lifter_ = 64;
    ReadyBuffer out_;
    InputWindow in_;
    int64_t m_ = 0, prevA_ = 0, lastReset_ = -100;
    bool havePrev_ = false;
};

// ------------------------------------------------------------------------------------------ PSOLA
// élastique's soloist modes stand-in, for one voice or instrument.
class Psola : public TimeStretcher {
public:
    Psola(const StretchParams& p, bool speech) : alpha_(p.alpha), pitch_(p.pitch > 0 ? p.pitch : 1.0)
    {
        const double lowHz = speech ? 60.0 : 40.0, highHz = speech ? 600.0 : 1500.0;
        dec_ = std::max(1, p.rate / 12000);
        maxT_ = (int)std::ceil(p.rate / lowHz);
        minT_ = std::max(2 * dec_, (int)std::floor(p.rate / highHz));
        unvoicedT_ = (int)std::lround(p.rate * (speech ? 0.005 : 0.008));
        threshold_ = speech ? 0.2 : 0.15;
        estimateEvery_ = std::max(1, p.rate / 250);
        maxHalf_ = std::max(maxT_, unvoicedT_);
        ts_ = -2.0 * maxHalf_;
        emitted_ = 0;
        accBase_ = (int64_t)std::floor(ts_) - maxHalf_ - 2;
    }

    void process(float* l, float* r, int n, const StretchPull& pull) override
    {
        int done = 0;
        while (done < n) {
            // what is finished: everything before where the next grain can start
            const int64_t complete = (int64_t)std::floor(ts_) - maxHalf_ - 1;
            if (emitted_ < complete) {
                const int k = (int)std::min<int64_t>(n - done, complete - emitted_);
                for (int j = 0; j < k; ++j) {
                    const size_t i = (size_t)(emitted_ + j - accBase_);
                    const float w = std::max(accW_[i], 0.5f);
                    l[done + j] = accL_[i] / w;
                    r[done + j] = accR_[i] / w;
                }
                emitted_ += k;
                done += k;
                // drop what was handed out
                const size_t drop = (size_t)(emitted_ - accBase_);
                if (drop > 16384) {
                    accL_.erase(accL_.begin(), accL_.begin() + (std::ptrdiff_t)drop);
                    accR_.erase(accR_.begin(), accR_.begin() + (std::ptrdiff_t)drop);
                    accW_.erase(accW_.begin(), accW_.begin() + (std::ptrdiff_t)drop);
                    accBase_ = emitted_;
                }
            } else {
                grain(pull);
            }
        }
    }

private:
    struct Mark { int64_t pos; int period; bool voiced; };

    // the mono signal (left + right) of [from, from + n) decimated by dec_ (box average) into out
    void decimated(int64_t from, int n, std::vector<float>& out) const
    {
        out.resize((size_t)n);
        for (int j = 0; j < n; ++j) {
            const float *a = in_.l(from + (int64_t)j * dec_), *b = in_.r(from + (int64_t)j * dec_);
            float s = 0;
            for (int q = 0; q < dec_; ++q) s += a[q] + b[q];
            out[(size_t)j] = s;
        }
    }

    // The pitch period at 'pos' (YIN on the decimated signal), 0 if there is none.
    int period(int64_t pos, const StretchPull& pull)
    {
        if (haveEstimate_ && pos >= lastEstimateAt_ && pos - lastEstimateAt_ < estimateEvery_) return lastEstimate_;
        lastEstimate_ = estimate(pos, pull);
        lastEstimateAt_ = pos;
        haveEstimate_ = true;
        return lastEstimate_;
    }

    int estimate(int64_t pos, const StretchPull& pull)
    {
        const int W = maxT_ / dec_ + 1, taus = maxT_ / dec_ + 2;
        const int64_t from = pos - (int64_t)W * dec_ / 2;
        in_.need(needLo_, from + (int64_t)(W + taus + 2) * dec_ + dec_, pull);
        decimated(from, W + taus + 2, x_);
        double e = 0;
        for (int j = 0; j < W; ++j) e += (double)x_[(size_t)j] * x_[(size_t)j];
        if (e < 1e-8 * W) return 0;
        d_.assign((size_t)taus, 0.0);
        double running = 0;
        int best = -1;
        double bestV = 1e300;
        const int tmin = std::max(1, minT_ / dec_);
        for (int t = 1; t < taus; ++t) {
            double s = 0;
            for (int j = 0; j < W; ++j) {
                const double v = (double)x_[(size_t)j] - x_[(size_t)(j + t)];
                s += v * v;
            }
            running += s;
            d_[(size_t)t] = s * t / (running + 1e-12);
        }
        for (int t = tmin; t < taus - 1; ++t) {
            if (d_[(size_t)t] < threshold_) {
                // the bottom of this dip
                while (t + 1 < taus - 1 && d_[(size_t)t + 1] < d_[(size_t)t]) ++t;
                best = t;
                break;
            }
            if (d_[(size_t)t] < bestV) { bestV = d_[(size_t)t]; best = -1 - t; }
        }
        if (best < 0) return 0;
        // between samples
        double t = best;
        if (best > 1 && best + 1 < taus) {
            const double a = d_[(size_t)best - 1], b = d_[(size_t)best], c = d_[(size_t)best + 1];
            const double den = a - 2 * b + c;
            if (den > 1e-12) t += 0.5 * (a - c) / den;
        }
        return std::max(minT_, std::min(maxT_, (int)std::lround(t * dec_)));
    }

    // where, within +/- T/5 of 'cand', a period best lines up with the one at 'prev'
    int64_t lineUp(int64_t prev, int64_t cand, int T)
    {
        const int half = T / 2, range = std::max(1, T / 5);
        auto score = [&](int64_t c) {
            double dot = 0, ec = 0;
            for (int j = -half; j < half; j += dec_) {
                const double a = (double)*in_.l(prev + j) + *in_.r(prev + j), b = (double)*in_.l(c + j) + *in_.r(c + j);
                dot += a * b;
                ec += b * b;
            }
            return dot / std::sqrt(ec + 1e-12);
        };
        int64_t best = cand;
        double bv = -1e300;
        for (int64_t c = cand - range; c <= cand + range; c += dec_) {
            const double s = score(c);
            if (s > bv) { bv = s; best = c; }
        }
        const int64_t centre = best;
        for (int64_t c = centre - dec_ + 1; c <= centre + dec_ - 1; ++c) {
            const double s = score(c);
            if (s > bv) { bv = s; best = c; }
        }
        return best;
    }

    void nextMark(const StretchPull& pull)
    {
        if (marks_.empty()) {
            const int64_t at = (int64_t)std::floor(ts_ / alpha_);
            needLo_ = at - 3 * maxHalf_;
            in_.need(needLo_, at + 3 * maxHalf_, pull);
            const int T = period(at, pull);
            marks_.push_back({at, T > 0 ? T : unvoicedT_, T > 0});
            return;
        }
        const Mark last = marks_.back();
        int64_t cand = last.pos + last.period;
        in_.need(needLo_, cand + 4 * maxHalf_, pull);
        const int T = period(cand, pull);
        if (T > 0) {
            if (last.voiced) {
                // keep the marks on the same point of each period
                cand = lineUp(last.pos, last.pos + T, T);
                if (cand <= last.pos) cand = last.pos + T;
            }
            marks_.push_back({cand, T, true});
        } else {
            marks_.push_back({cand, unvoicedT_, false});
        }
    }

    void grain(const StretchPull& pull)
    {
        const double ta = ts_ / alpha_;
        while (marks_.empty() || (double)marks_.back().pos < ta + maxHalf_) nextMark(pull);
        // the mark nearest where the stretch says
        size_t best = 0;
        double bd = 1e300;
        for (size_t i = 0; i < marks_.size(); ++i) {
            const double d = std::fabs((double)marks_[i].pos - ta);
            if (d < bd) { bd = d; best = i; }
        }
        const Mark mk = marks_[best];
        // forget marks well behind
        while (marks_.size() > 2 && (double)marks_.front().pos < ta - 4.0 * maxHalf_) marks_.pop_front();
        needLo_ = std::max(needLo_, marks_.front().pos - 2 * maxHalf_);
        const int T = mk.period;
        in_.need(std::min(needLo_, mk.pos - T), mk.pos + T + 1, pull);
        // lay it down centred on the next output position
        const int64_t centre = (int64_t)std::llround(ts_);
        const int64_t start = centre - T;
        const int64_t endOut = centre + T;
        if (endOut - accBase_ > (int64_t)accL_.size()) {
            const size_t n = (size_t)(endOut - accBase_) + 4096;
            accL_.resize(n, 0.f);
            accR_.resize(n, 0.f);
            accW_.resize(n, 0.f);
        }
        const float *gl = in_.l(mk.pos - T), *gr = in_.r(mk.pos - T);
        for (int j = 0; j < 2 * T; ++j) {
            const int64_t at = start + j;
            if (at < accBase_) continue;
            const float w = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * (j + 0.5) / (2.0 * T)));
            const size_t i = (size_t)(at - accBase_);
            accL_[i] += w * gl[j];
            accR_[i] += w * gr[j];
            accW_[i] += w;
        }
        ts_ += mk.voiced ? (double)T / pitch_ : (double)T;
    }

    double alpha_, pitch_;
    int dec_ = 1, maxT_ = 0, minT_ = 0, unvoicedT_ = 0, maxHalf_ = 0;
    double threshold_ = 0.15;
    int64_t lastEstimateAt_ = 0, estimateEvery_ = 192;
    int lastEstimate_ = 0;
    bool haveEstimate_ = false;
    double ts_ = 0;
    int64_t emitted_ = 0, accBase_ = 0, needLo_ = 0;
    std::vector<float> accL_, accR_, accW_, x_;
    std::vector<double> d_;
    std::deque<Mark> marks_;
    InputWindow in_;
};


// ------------------------------------------------------------------------------------------ delay
// A stretcher whose sound comes later (or, negative, earlier) by 'delay' output samples.
class Delayed : public TimeStretcher {
public:
    Delayed(std::unique_ptr<TimeStretcher> inner, double delay, double alpha)
        : inner_(std::move(inner)), shiftIn_((int64_t)std::llround(delay / alpha)) {}
    void process(float* l, float* r, int n, const StretchPull& pull) override
    {
        const int64_t s = shiftIn_;
        const StretchPull back = [&](int64_t i, int k, float* a, float* b) { pull(i - s, k, a, b); };
        inner_->process(l, r, n, back);
    }

private:
    std::unique_ptr<TimeStretcher> inner_;
    int64_t shiftIn_;
};

}  // namespace

// ------------------------------------------------------------------------------------------ public
const std::vector<ClassicPreset>& classicPresets() { return kClassic; }

const ClassicPreset* classicPreset(const std::string& id)
{
    for (const auto& p : kClassic)
        if (id == p.id) return &p;
    return nullptr;
}

double elastiqueDelaySamples(const std::string& mode, double semitones)
{
    if (!std::isfinite(semitones)) return 0.0;
    double latency = 900.0, reach = 2.0;                        // Soloist (both modes)
    if (mode == "pro") { latency = 2500.0; reach = 8.0; }
    else if (mode == "efficient") { latency = 1050.0; reach = 4.0; }
    const double a = std::fabs(semitones);
    const double s = std::min(a, reach) * (semitones < 0 ? -1.0 : 1.0);
    const double taper = a <= reach ? 1.0 : std::max(0.0, 1.0 - (a - reach) / 3.0);      // 3 semitones further it is gone
    return latency * (std::pow(2.0, s / 12.0) - 1.0) * taper;
}

const std::vector<std::pair<std::string, std::string>>& elastiqueModes()
{
    static const std::vector<std::pair<std::string, std::string>> m = {
        {"pro", "Pro"}, {"efficient", "Efficient"}, {"soloist-mono", "Soloist (Monophonic)"}, {"soloist-speech", "Soloist (Speech)"}};
    return m;
}

bool stretchKeepsFormants(const std::string& method, const std::string& mode, bool preserveFormants)
{
    return preserveFormants && method == "elastique" && (mode == "pro" || mode == "soloist-mono");
}

bool stretcherShiftsPitch(const std::string& method, const std::string& mode, bool preserveFormants)
{
    if (method != "elastique") return false;
    return mode == "soloist-speech" || (mode == "soloist-mono" && preserveFormants);
}

static std::unique_ptr<TimeStretcher> makeStretcherPlain(const StretchParams& p)
{
    const double alpha = (std::isfinite(p.alpha) && p.alpha > 1e-3) ? std::min(p.alpha, 1000.0) : 1.0;
    StretchParams q = p;
    q.alpha = alpha;
    q.rate = std::max(1000, p.rate);
    if (p.method == "classic") {
        const ClassicPreset* pr = classicPreset(p.mode);
        if (!pr) pr = classicPreset("A03");
        return std::unique_ptr<TimeStretcher>(new ClassicStretcher(*pr, q.rate, alpha, p.inputEnd));
    }
    if (p.mode == "soloist-mono" || p.mode == "soloist-speech") {
        if (!stretcherShiftsPitch(p.method, p.mode, p.preserveFormants)) q.pitch = 1.0;
        return std::unique_ptr<TimeStretcher>(new Psola(q, p.mode == "soloist-speech"));
    }
    return std::unique_ptr<TimeStretcher>(new PhaseVocoder(q, p.mode != "efficient"));
}

std::unique_ptr<TimeStretcher> makeStretcher(const StretchParams& p)
{
    std::unique_ptr<TimeStretcher> st = makeStretcherPlain(p);
    const double d = p.delay;
    if (p.method == "classic" || !(std::fabs(d) > 0)) return st;
    return std::unique_ptr<TimeStretcher>(new Delayed(std::move(st), d, std::isfinite(p.alpha) && p.alpha > 1e-3 ? std::min(p.alpha, 1000.0) : 1.0));
}

}  // namespace reno
