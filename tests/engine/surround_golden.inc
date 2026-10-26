// A stereo project that uses most of the mixer (shared by the golden program and surround_test): its render's
// FNV-1a hash over the float bits must not change.
struct GoldSynth : public MediaReader {
    MediaInfo mi;
    double freq = 220;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const double t = (double)n / sampleRate;
            left[k] = n >= 0 && t < 10 ? (float)(0.5 * std::sin(2 * 3.14159265358979 * freq * t)) : 0.f;
            right[k] = n >= 0 && t < 10 ? (float)(0.4 * std::sin(2 * 3.14159265358979 * freq * 1.5 * t + 0.3)) : 0.f;
        }
        return true;
    }
};
static std::unique_ptr<MediaReader> goldOpen(const std::string& path, std::string*)
{
    std::unique_ptr<GoldSynth> r(new GoldSynth());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 10;
    r->mi.sampleRate = 48000;
    r->mi.channels = 2;
    r->freq = std::atof(path.c_str() + 5);
    return std::unique_ptr<MediaReader>(r.release());
}
static Project goldProject()
{
    Project p;
    auto media = [&](const char* path) {
        MediaItem m;
        m.id = p.newId();
        m.path = path;
        std::string e;
        m.info = goldOpen(path, &e)->info();
        p.media.push_back(m);
        return m.id;
    };
    const uint64_t m1 = media("gold:220"), m2 = media("gold:330"), m3 = media("gold:97");
    Bus b;
    b.id = p.newId();
    b.name = "A";
    b.level = 0.8;
    b.pan = -0.3;
    p.buses.push_back(b);
    const uint64_t busA = b.id;
    for (int i = 0; i < 3; ++i) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        t.level = 0.9 - 0.2 * i;
        t.pan = -0.5 + 0.5 * i;
        Event e;
        e.id = p.newId();
        e.mediaId = i == 0 ? m1 : (i == 1 ? m2 : m3);
        e.start = 0.1 * i;
        e.length = 2.0;
        e.offset = 0.25;
        e.fadeIn = 0.2;
        e.fadeOut = 0.3;
        e.gain = 0.7;
        if (i == 1) e.channels = "swap";
        t.events.push_back(e);
        if (i == 1) {
            Envelope env;
            env.kind = "pan";
            env.points = {{0.0, -0.8, CurveLinear}, {1.5, 0.6, CurveSmooth}};
            t.envelopes.push_back(env);
            Send s;
            s.bus = busA;
            s.level = 0.5;
            t.sends.push_back(s);
        }
        if (i == 2) {
            t.output = busA;
            Envelope env;
            env.kind = "volume";
            env.points = {{0.0, 1.0, CurveFast}, {1.0, 0.2, CurveLinear}};
            t.envelopes.push_back(env);
        }
        p.tracks.push_back(t);
    }
    p.masterVolume = 0.85;
    p.masterPan = 0.2;
    return p;
}
static uint64_t goldHash(const Project& p, FxRuntime& fx)
{
    MediaPool pool;
    pool.setOpener(goldOpen);
    AudioEngine eng(pool, fx);
    uint64_t h = 1469598103934665603ull;
    std::vector<float> l(4800), r(4800);
    for (int64_t s = 0; s < 48000 * 2; s += 4800) {
        eng.render(p, s, 4800, l.data(), r.data(), true);
        for (int k = 0; k < 4800; ++k) {
            uint32_t a, b;
            std::memcpy(&a, &l[k], 4);
            std::memcpy(&b, &r[k], 4);
            h = (h ^ a) * 1099511628211ull;
            h = (h ^ b) * 1099511628211ull;
        }
    }
    return h;
}
