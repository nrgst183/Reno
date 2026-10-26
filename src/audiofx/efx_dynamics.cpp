// Reno's eFX Limiter and Compressor (see efx.h), written from MAGIX's manual and the behaviour of the presets, with
// the real ones' 31 settings.
#include "efx.h"
#include <cstring>

namespace reno {
namespace efx {
namespace {


// A hard-or-soft clip: the value is the share of the hard (rectangular) curve; the soft one is a sine curve that
// rounds the top off. Never above 1.
inline float clipShape(float x, float hard)
{
    const float h = std::max(-1.f, std::min(1.f, x));
    if (hard >= 0.999f) return h;
    const float lim = 1.5707963f;
    const float s = std::sin(std::max(-lim, std::min(lim, x)));
    return hard * h + (1.f - hard) * s;
}

// ======================================================================================================= Limiter

// A look-ahead peak limiter: the level that would take the sound over the threshold is found on the incoming sound,
// the sound is held back by the attack time, and the gain ramps down so it is complete when the peak arrives (nothing
// passes the threshold), then recovers with the release time, longer after long, heavy work (adaptive release).
// Input and sidechain low cuts, peak / RMS mix and the clipper (gain, hard / soft shape) are the real one's controls.
// Auto makeup raises the threshold to full scale; the output setting is the ceiling.
class Limiter : public EfxPlugin {
public:
    Limiter() : EfxPlugin("eFX Limiter", 0x4D583241u, limiterDefs()) {}

    // the look-ahead is the attack time
    int latencySamples() override { return std::max(1, (int)std::lround(real(20) * 0.001 * sr_)) - 1; }

protected:
    void prepare() override
    {
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        rmsCoef_ = 1.0 - std::exp(-1.0 / (0.005 * sr_));
        stressUp_ = 1.0 - std::exp(-1.0 / (0.05 * sr_));
        stressDown_ = 1.0 - std::exp(-1.0 / (0.5 * sr_));
        inLcOn_ = scLcOn_ = false;
        inLcF_ = scLcF_ = 0;
        setup(1);
    }
    void reset() override
    {
        setup(n_);
        for (int c = 0; c < 2; ++c) { hpIn_[c].reset(); hpSc_[c].reset(); }
        msq_ = 0;
        stress_ = 0;
        relPrev_ = 1.0;
        gIn_.primed = gOut_.primed = false;
    }

    void run(float* l, float* r, int n) override
    {
        // the settings, read once per block
        const double attackMs = real(20);
        const int want = std::max(1, (int)std::lround(attackMs * 0.001 * sr_));
        if (want != n_) setup(want);
        const double thrDb = real(5);
        const double thr = dbToLin(thrDb);
        const double makeup = on(23) ? 1.0 / thr : 1.0;
        const double relMs = real(6);
        const double adaptive = real(21) / 100.0;
        const double rmsMix = real(22) / 100.0;
        const double clipG = std::max(1.0, dbToLin(real(7)));
        const float hard = (float)(real(26) / 100.0);
        const double inLc = real(24), scLc = real(25);
        const bool useIn = inLc >= 0.5, useSc = scLc >= 0.5;
        if (useIn != inLcOn_ || (useIn && inLc != inLcF_)) {
            inLcOn_ = useIn; inLcF_ = inLc;
            for (int c = 0; c < 2; ++c) if (useIn) hpIn_[c].highpass(sr_, inLc);
        }
        if (useSc != scLcOn_ || (useSc && scLc != scLcF_)) {
            scLcOn_ = useSc; scLcF_ = scLc;
            for (int c = 0; c < 2; ++c) if (useSc) hpSc_[c].highpass(sr_, scLc);
        }
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        // release time = how long the gain needs to come most of the way back: half the one-pole time constant.
        // Fitted to VEGAS's render of a sample project, where the Limiters squash a signal 30 dB above threshold
        const double relBase = std::max(0.2, relMs) * 0.5 * 0.001 * sr_;
        const double kneeDb = 2.0;

        for (int k = 0; k < n; ++k) {
            const float gi = (float)gIn_.next();
            float xl = l[k] * gi, xr = r[k] * gi;
            if (inLcOn_) { xl = hpIn_[0].process(xl); xr = hpIn_[1].process(xr); }
            // level the limiter watches
            float dl = xl, dr = xr;
            if (scLcOn_) { dl = hpSc_[0].process(dl); dr = hpSc_[1].process(dr); }
            const double peak = std::max(std::fabs(dl), std::fabs(dr));
            msq_ += (0.5 * ((double)dl * dl + (double)dr * dr) - msq_) * rmsCoef_;
            const double det = rmsMix <= 0 ? peak : (1.0 - rmsMix) * peak + rmsMix * std::sqrt(2.0 * msq_);
            // gain that keeps it at the threshold, with a soft knee
            double need = 1.0;
            if (det > thr * 0.8) {
                const double over = 20.0 * std::log10(det / thr + 1e-12);
                double gr = 0;
                if (over >= kneeDb * 0.5) gr = -over;
                else if (over > -kneeDb * 0.5) gr = -(over + kneeDb * 0.5) * (over + kneeDb * 0.5) / (2 * kneeDb);
                need = std::pow(10.0, gr / 20.0);
            }
            // lowest gain asked for in the next 'n_' samples (the sound is held back by as many)
            const double low = slidingMin(need);
            // back up with the release time, longer the more the limiter has worked
            const double depth = std::min(1.0, (1.0 - low) * 4.0);
            stress_ += (depth - stress_) * (depth > stress_ ? stressUp_ : stressDown_);
            double rel;
            if (low <= relPrev_) rel = low;
            else {
                const double t = relBase * (1.0 + adaptive * 5.0 * stress_);
                rel = relPrev_ + (low - relPrev_) * (1.0 - std::exp(-1.0 / t));
            }
            relPrev_ = rel;
            // ramp down over the attack time: average of the last n_ gains
            sum_ += rel - ring_[ringPos_];
            ring_[ringPos_] = rel;
            if (++ringPos_ >= n_) {
                ringPos_ = 0;
                sum_ = 0;
                for (int i = 0; i < n_; ++i) sum_ += ring_[(size_t)i];       // summed afresh now and then to avoid drift
            }
            const double g = sum_ / n_;
            // the delayed sound
            float yl, yr;
            if (n_ > 1) {
                yl = dline_[0][(size_t)dpos_]; yr = dline_[1][(size_t)dpos_];
                dline_[0][(size_t)dpos_] = xl; dline_[1][(size_t)dpos_] = xr;
                if (++dpos_ >= n_ - 1) dpos_ = 0;
            } else { yl = xl; yr = xr; }
            float zl = (float)(yl * g * makeup), zr = (float)(yr * g * makeup);
            // clipper: boosted by the clip gain, shaped, never beyond full scale
            if (clipG > 1.0001) {
                zl = clipShape(zl * (float)clipG, hard);
                zr = clipShape(zr * (float)clipG, hard);
            }
            zl = std::max(-1.f, std::min(1.f, zl));
            zr = std::max(-1.f, std::min(1.f, zr));
            const float go = (float)gOut_.next();
            l[k] = zl * go;
            r[k] = zr * go;
        }
        for (int c = 0; c < 2; ++c) { hpIn_[c].z1 = flushDenormal((float)hpIn_[c].z1); hpSc_[c].z1 = flushDenormal((float)hpSc_[c].z1); }
    }

private:
    void setup(int n)
    {
        n_ = std::max(1, n);
        ring_.assign((size_t)n_, 1.0);
        ringPos_ = 0;
        sum_ = (double)n_;
        for (int c = 0; c < 2; ++c) dline_[c].assign((size_t)std::max(1, n_ - 1), 0.f);
        dpos_ = 0;
        qIdx_.assign((size_t)n_ + 2, 0);
        qVal_.assign((size_t)n_ + 2, 1.0);
        qHead_ = qTail_ = 0;
        qCount_ = 0;
        t_ = 0;
        relPrev_ = 1.0;
    }

    // smallest of the last n_ values
    double slidingMin(double v)
    {
        const size_t cap = qIdx_.size();
        while (qCount_ > 0) {
            const size_t last = (qTail_ + cap - 1) % cap;
            if (qVal_[last] >= v) { qTail_ = last; --qCount_; } else break;
        }
        qIdx_[qTail_] = t_;
        qVal_[qTail_] = v;
        qTail_ = (qTail_ + 1) % cap;
        ++qCount_;
        while (qCount_ > 0 && qIdx_[qHead_] <= t_ - n_) { qHead_ = (qHead_ + 1) % cap; --qCount_; }
        ++t_;
        return qVal_[qHead_];
    }

    int n_ = 1;
    std::vector<double> ring_;
    int ringPos_ = 0;
    double sum_ = 1;
    std::vector<float> dline_[2];
    int dpos_ = 0;
    std::vector<int64_t> qIdx_;
    std::vector<double> qVal_;
    size_t qHead_ = 0, qTail_ = 0, qCount_ = 0;
    int64_t t_ = 0;
    double relPrev_ = 1.0, msq_ = 0, stress_ = 0;
    double rmsCoef_ = 0.01, stressUp_ = 0.001, stressDown_ = 0.0001;
    Smooth gIn_, gOut_;
    Biquad hpIn_[2], hpSc_[2];
    bool inLcOn_ = false, scLcOn_ = false;
    double inLcF_ = 0, scLcF_ = 0;
};

// ===================================================================================================== Compressor

// A compressor with a soft knee and gain moving in decibels. It watches the louder of the two channels (so the
// stereo image does not lean); the reduction follows the attack and release times, the release growing with how hard
// and long the compressor has worked (adaptive release). Mix lets the dry sound back in; auto makeup restores the
// level a full-scale signal loses; soft clip catches overs at the output.
class Compressor : public EfxPlugin {
public:
    Compressor() : EfxPlugin("eFX Compressor", 0x4D583237u, compressorDefs()) {}

protected:
    void prepare() override
    {
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        mix_.setTime(sr_, 5);
        stressUp_ = 1.0 - std::exp(-1.0 / (0.1 * sr_));
        stressDown_ = 1.0 - std::exp(-1.0 / (1.0 * sr_));
    }
    void reset() override
    {
        grState_ = 0;
        stress_ = 0;
        for (int c = 0; c < 2; ++c) sc_[c].reset();
        gIn_.primed = gOut_.primed = mix_.primed = false;
    }

    void run(float* l, float* r, int n) override
    {
        const double thrDb = real(6);
        const double ratio = std::max(1.0, real(7));
        const double attackS = std::max(0.00005, real(8) * 0.001);
        const double releaseS = std::max(0.001, real(9) * 0.001);
        const double adaptive = real(20);
        const bool autoMakeup = on(21);
        const bool listen = on(10);
        const bool softClip = on(11);
        const int type = choice(15);
        const double scFreq = real(5);
        if (type != scType_ || scFreq != scFreq_) {
            scType_ = type; scFreq_ = scFreq;
            for (int c = 0; c < 2; ++c) {
                if (type == 1) sc_[c].lowpass(sr_, scFreq);
            }
        }
        const bool filtered = type != 0;
        const double makeupDb = autoMakeup ? -thrDb * (1.0 - 1.0 / ratio) : 0.0;
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        mix_.set(real(22) / 100.0);
        const double attackCoef = std::exp(-1.0 / (attackS * sr_));
        const double kneeDb = 10.0;
        const double slope = 1.0 / ratio - 1.0;

        for (int k = 0; k < n; ++k) {
            const float gi = (float)gIn_.next();
            const float xl = l[k] * gi, xr = r[k] * gi;
            float dl = xl, dr = xr;
            if (filtered) { dl = sc_[0].process(dl); dr = sc_[1].process(dr); }
            const double level = std::max(std::fabs(dl), std::fabs(dr));
            const double ldb = level > 1e-9 ? 20.0 * std::log10(level) : -180.0;
            const double over = ldb - thrDb;
            double target = 0;
            if (over > kneeDb * 0.5) target = slope * over;
            else if (over > -kneeDb * 0.5) target = slope * (over + kneeDb * 0.5) * (over + kneeDb * 0.5) / (2.0 * kneeDb);
            double coef;
            if (target < grState_) coef = attackCoef;
            else {
                const double t = releaseS * (1.0 + adaptive * 0.6 * stress_);
                coef = std::exp(-1.0 / (t * sr_));
            }
            grState_ = target + coef * (grState_ - target);
            const double depth = std::min(1.0, -grState_ / 12.0);
            stress_ += (depth - stress_) * (depth > stress_ ? stressUp_ : stressDown_);
            const double gain = std::pow(10.0, (grState_ + makeupDb) / 20.0);
            const double m = mix_.next();
            const double go = gOut_.next();
            float ol, orr;
            if (listen) { ol = dl; orr = dr; }
            else {
                ol = (float)((xl * (1.0 - m) + xl * gain * m));
                orr = (float)((xr * (1.0 - m) + xr * gain * m));
            }
            ol = (float)(ol * go);
            orr = (float)(orr * go);
            if (softClip) { ol = softClipOut(ol); orr = softClipOut(orr); }
            l[k] = ol;
            r[k] = orr;
        }
    }

private:
    static float softClipOut(float x)
    {
        const float a = std::fabs(x);
        if (a <= 0.5f) return x;
        const float y = 0.5f + 0.5f * std::tanh((a - 0.5f) / 0.5f);
        return x < 0 ? -y : y;
    }

    double grState_ = 0, stress_ = 0, stressUp_ = 0.001, stressDown_ = 0.0001;
    Smooth gIn_, gOut_, mix_;
    Biquad sc_[2];
    int scType_ = -1;
    double scFreq_ = -1;
};

}  // namespace

std::unique_ptr<EfxPlugin> makeLimiter() { return std::unique_ptr<EfxPlugin>(new Limiter()); }
std::unique_ptr<EfxPlugin> makeCompressor() { return std::unique_ptr<EfxPlugin>(new Compressor()); }

}  // namespace efx
}  // namespace reno
