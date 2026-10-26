// Tests of the built-in analysis effects (src/ofx/builtins_analysis*.cpp): Stabilize Video, Video Stabilization (Magix),
// Motion Tracker, Flicker Control, Denoise (NLM), Smart Noise Reduction, Colorization, Style Transfer.
//
//   analysis_test
//
// Covers the definitions, the pictures, render scale 1/2 against full size, threads against one thread, and the
// engine / OpenFX host / .veg round trip.
#define main fxscale_main
#include "fxscale_test.cpp"
#undef main
#include "../../src/vegas/vegas.h"
#include <thread>

typedef std::map<std::string, std::string> StrMap;
typedef std::vector<Buf> Seq;

static const char* kIds[] = {"com.vegascreativesoftware:Stabilize", "de.magix:Stabilize", "de.magix:MotionTracker", "de.magix:FlickerReducer",
                             "de.magix:DenoisingNLM", "com.sonycreativesoftware:smart_noise_reduction", "de.magix:Colorization", "de.magix:StyleTransfer"};

// One render of frame 't' of a sequence (the effect may fetch the others), as instance 'key'.
static Buf runSeq(const Def& def, const Params& over, const StrMap& sover, const Seq& seq, int t, double scale, const void* key)
{
    Params store;
    StrMap strs;
    for (const PDef& p : def.params) {
        if (p.type == PT::Group) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : over) store[kv.first] = kv.second;
    for (const auto& kv : sover) strs[kv.first] = kv.second;
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
    c.time = t;
    c.fps = 30;
    c.src = seq[(size_t)t];
    c.out.resize(c.src.w, c.src.h);
    c.frameW = c.src.w;
    c.frameH = c.src.h;
    c.scale = scale;
    c.instance = key;
    const Seq* sp = &seq;
    c.fetch = [sp](double time, Buf& into) {
        const int k = (int)std::lround(time);
        if (k < 0 || k >= (int)sp->size()) return false;
        into = (*sp)[(size_t)k];
        return true;
    };
    def.render(c);
    return c.out;
}

static Seq renderAll(const Def& def, const Params& over, const StrMap& sover, const Seq& seq, double scale, const void* key)
{
    Seq o;
    for (int t = 0; t < (int)seq.size(); ++t) o.push_back(runSeq(def, over, sover, seq, t, scale, key));
    if (def.destroy) def.destroy(key);
    return o;
}

static Buf reduceBuf(const Buf& b, int f) { return reduce(b, f); }
static Seq reduceSeq(const Seq& s, int f)
{
    Seq o;
    for (const Buf& b : s) o.push_back(reduceBuf(b, f));
    return o;
}

// ------------------------------------------------------------------ sequences
static int shakeX(int t) { return (int)std::lround(7.0 * std::sin(t * 1.3 + 0.4)); }
static int shakeY(int t) { return (int)std::lround(5.0 * std::sin(t * 2.1 + 1.0)); }

// A bigger picture (with detail) cropped at a position that moves by a few pixels each frame.
static Seq shaky(int W, int H, int n)
{
    const int M = 24;
    const Buf big = picture(W + 2 * M, H + 2 * M, std::getenv("STRIPES") ? 1 : 0);
    Seq s;
    for (int t = 0; t < n; ++t) {
        Buf b;
        b.resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) std::memcpy(b.at(x, y), big.at(x + M + shakeX(t), y + M + shakeY(t)), 4 * sizeof(float));
        s.push_back(b);
    }
    return s;
}

static std::vector<float> grayOf(const Buf& b)
{
    std::vector<float> g((size_t)b.w * b.h);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const float* p = b.at(x, y);
            g[(size_t)y * b.w + x] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        }
    return g;
}

// How far picture b is moved against a (whole pixels, central part compared, within +-R).
static void shiftBetween(const Buf& a, const Buf& b, int R, int& bx, int& by)
{
    const std::vector<float> ga = grayOf(a), gb = grayOf(b);
    double best = 1e30;
    bx = by = 0;
    const int m = R + 2;
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
            double s = 0;
            for (int y = m; y < a.h - m; y += 2)
                for (int x = m; x < a.w - m; x += 2) {
                    const double d = ga[(size_t)y * a.w + x] - gb[(size_t)(y + dy) * a.w + x + dx];
                    s += d * d;
                }
            if (s < best) { best = s; bx = dx; by = dy; }
        }
}

// Mean frame-to-frame motion (pixels) of a sequence.
static double jitter(const Seq& s, int from = 0)
{
    double sum = 0;
    int n = 0;
    for (size_t t = (size_t)from; t + 1 < s.size(); ++t) {
        int dx, dy;
        shiftBetween(s[t], s[t + 1], 12, dx, dy);
        sum += std::sqrt((double)(dx * dx + dy * dy));
        ++n;
    }
    return n ? sum / n : 0;
}

static double meanLuma(const Buf& b)
{
    double s = 0;
    const std::vector<float> g = grayOf(b);
    for (float v : g) s += v;
    return s / (double)g.size();
}

static uint32_t gSeed = 12345;
static float rnd()
{
    gSeed = gSeed * 1664525u + 1013904223u;
    return (float)((gSeed >> 8) & 0xFFFF) / 65536.f - 0.5f;
}

static double psnr(const Buf& a, const Buf& b)
{
    double s = 0;
    for (size_t i = 0; i < a.px.size(); ++i) { const double d = a.px[i] - b.px[i]; s += d * d; }
    s /= (double)a.px.size();
    return 10.0 * std::log10(1.0 / std::max(s, 1e-12));
}

// ------------------------------------------------------------------ 1. definitions
static const PDef* param(const Def& d, const char* name)
{
    for (const PDef& p : d.params) if (p.name == name) return &p;
    return nullptr;
}

static void defTests()
{
    for (const char* id : kIds) {
        const Def* d = findDef(id);
        check(d != nullptr, std::string(id) + ": there is a built-in");
        if (!d) continue;
        check(!d->description.empty() && d->render && d->multiRes, std::string(id) + ": described, renders, multi-resolution");
        const bool temporal = std::string(id) != "de.magix:Colorization" && std::string(id) != "de.magix:StyleTransfer" && std::string(id) != "de.magix:MotionTracker" &&
                              std::string(id) != "com.sonycreativesoftware:smart_noise_reduction" && std::string(id) != "de.magix:DenoisingNLM";
        if (temporal) check(d->temporal && d->destroy, std::string(id) + ": asks for the other frames and drops its cache");
    }
    const Def* s = findDef("de.magix:Stabilize");
    if (s) {
        struct E { const char* n; double v; };
        const E want[] = {{"Presets", 1}, {"MotionCompensation", 2}, {"XScale", 0.85}, {"YScale", 0.8}, {"NumGridsX", 14}, {"NumGridsY", 9}, {"PointsPerGridCell", 2},
                          {"SmoothingMagnitude", 2.5}, {"AvoidBlackBorders", 1}, {"MotionBlurThreshold", 10}, {"MotionBlurMaxMagnitude", 19}, {"GeometricModel", 4},
                          {"TrackerWinSize", 16}, {"TrackerLevel", 4}, {"SmoothingFrameLength", 75}};
        for (const E& e : want) {
            const PDef* p = param(*s, e.n);
            check(p && std::fabs(p->def[0] - e.v) < 1e-9, std::string("Video Stabilization ") + e.n + " default", p ? strfmt("%g", p->def[0]) : "missing");
        }
    }
    const Def* v = findDef("com.vegascreativesoftware:Stabilize");
    if (v) {
        const PDef *a = param(*v, "PanSmoothing"), *b = param(*v, "ViewScale"), *c = param(*v, "RollingShutterCorrection");
        check(a && b && c && a->def[0] == 0.5 && b->def[0] == 0.5, "Stabilize Video: PanSmoothing and ViewScale 0.5, RollingShutterCorrection present");
    }
    const Def* f = findDef("de.magix:FlickerReducer");
    if (f) {
        const PDef *fr = param(*f, "Frequency"), *tw = param(*f, "TimeWindow"), *ty = param(*f, "Type");
        check(fr && tw && ty && fr->def[0] == 55 && tw->def[0] == 2 && ty->def[0] == 3, "Flicker Control: Frequency 55, TimeWindow 2, Type 3");
    }
    if (f) { const PDef* p = param(*f, "Mode"); check(p && p->choices.size() == 2 && p->choices[0] == "Reduction", "Flicker Control: Mode choices as VEGAS lists them"); }
    const Def* m = findDef("de.magix:MotionTracker");
    if (m) {
        const PDef *n = param(*m, "NoPoints"), *mm = param(*m, "MotionModel");
        check(n && mm && n->def[0] == 36 && mm->def[0] == 8, "Motion Tracker: NoPoints 36, MotionModel 8");
    }
    const Def* st = findDef("de.magix:StyleTransfer");
    if (st) {
        const PDef* p = param(*st, "Style");
        check(p && p->choices.size() == 17 && p->choices[0] == "Self Portrait (Picasso)" && p->choices[16] == "Rick and Morty", "Style Transfer: VEGAS's 17 styles in its order");
        const PDef* im = param(*st, "StyleRefImg");
        const PDef* cs = param(*st, "CustomSwitch");
        check(im && im->type == PT::Image && cs && cs->choices.size() == 2 && param(*st, "DetailSwitch") && param(*st, "Model not found"), "Style Transfer: the reference image, custom switch, detail level and model message");
    }
    // the buttons VEGAS has (pressing one is the changed hook: Analyze runs the analysis, Clear drops it)
    if (s) {
        const char* const names[] = {"AnalyzeMotionBasic", "CLearMotionBasic", "AnalyzeMotion", "CLearMotion"};
        bool all = s->changed != nullptr;
        for (const char* n : names) { const PDef* b = param(*s, n); all = all && b && b->type == PT::Button; }
        check(all, "Video Stabilization: the four buttons and a changed hook");
    }
    if (v) { const PDef* b = param(*v, "AnalyzeButton"); check(b && b->type == PT::Button && v->changed, "Stabilize Video: the Apply button"); }
    if (f) {
        const PDef* c1 = param(*f, "Calc");
        const PDef* c2 = param(*f, "Clear");
        const PDef* tw = param(*f, "TimeWindow");
        check(c1 && c2 && c1->type == PT::Button && c2->type == PT::Button && f->changed && tw && tw->type == PT::Double, "Flicker Control: Calculate and Reset buttons, TimeWindow a Double");
    }
    const Def* col = findDef("de.magix:Colorization");
    if (col) check(param(*col, "Localization") != nullptr, "Colorization: the Localization message choice");
}

// ------------------------------------------------------------------ 2. pictures
static void stabilizeTests()
{
    const Seq in = shaky(320, 180, 20);
    const double j0 = jitter(in);
    for (const char* id : {"de.magix:Stabilize", "com.vegascreativesoftware:Stabilize"}) {
        const Def* d = findDef(id);
        if (!d) continue;
        static int keyA, keyB;
        const Seq out = renderAll(*d, {}, {}, in, 1.0, &keyA);
        const double j1 = jitter(out);
        check(j1 < 0.5 * j0, std::string(id) + ": the shaking is reduced by half or more", strfmt("input %.2f px/frame, output %.2f", j0, j1));
        bool sane = true;
        for (const Buf& b : out) {
            if (meanLuma(b) < 0.05) sane = false;
            for (float v : b.px) if (!(v == v)) sane = false;
        }
        check(sane, std::string(id) + ": the pictures are not blank or broken");
        // a still camera stays as it is
        Seq still;
        for (int t = 0; t < 12; ++t) still.push_back(in[0]);
        const Seq so = renderAll(*d, {}, {}, still, 1.0, &keyB);
        double mx = 0;
        for (const Buf& b : so) mx = std::max(mx, compare(b, still[0]).mean);
        check(mx < 6.0, std::string(id) + ": a still camera is left (nearly) alone", strfmt("mean %.2f levels", mx));
    }
    // freeze: the picture holds on the first frame's view
    if (const Def* d = findDef("de.magix:Stabilize")) {
        static int keyC;
        const Seq out = renderAll(*d, {{"StabilizeMode", {1}}, {"AvoidBlackBorders", {0}}}, {}, in, 1.0, &keyC);
        const double j1 = jitter(out);
        if (std::getenv("DBG")) for (size_t q = 0; q < out.size(); ++q) { int sx, sy; shiftBetween(out[0], out[q], 12, sx, sy); std::printf("DBG freeze t%zu shift to t0 %d %d\n", q, sx, sy); }
        check(j1 < 0.4 * j0, "Video Stabilization freeze: the picture holds still", strfmt("input %.2f px/frame, output %.2f", j0, j1));
    }
}

static void flickerTests()
{
    const Def* d = findDef("de.magix:FlickerReducer");
    if (!d) return;
    Seq in;
    const Buf base = picture(240, 135, 0);
    for (int t = 0; t < 16; ++t) {
        Buf b = base;
        const float g = (t % 2) ? 0.7f : 1.0f;
        for (size_t i = 0; i < b.px.size(); i += 4) { b.px[i] *= g; b.px[i + 1] *= g; b.px[i + 2] *= g; }
        in.push_back(b);
    }
    static int key;
    const Seq out = renderAll(*d, {}, {}, in, 1.0, &key);
    auto spread = [](const Seq& s) {
        double lo = 1e9, hi = -1e9;
        for (size_t t = 2; t + 2 < s.size(); ++t) { const double m = meanLuma(s[t]); lo = std::min(lo, m); hi = std::max(hi, m); }
        return hi - lo;
    };
    const double a = spread(in), b = spread(out);
    check(b < 0.4 * a, "Flicker Control: alternating brightness is levelled", strfmt("spread %.4f -> %.4f", a, b));
    Seq steady(10, base);
    static int key2;
    const Seq so = renderAll(*d, {}, {}, steady, 1.0, &key2);
    double mx = 0;
    for (const Buf& x : so) mx = std::max(mx, compare(x, base).mean);
    check(mx < 1.0, "Flicker Control: a steady clip is left alone", strfmt("mean %.2f levels", mx));
}

static void denoiseTests()
{
    const Buf clean = picture(240, 135, 0);
    Buf noisy = clean;
    for (size_t i = 0; i < noisy.px.size(); i += 4) {
        if (noisy.px[i + 3] < 0.99f) continue;
        for (int k = 0; k < 3; ++k) noisy.px[i + k] = std::min(1.f, std::max(0.f, noisy.px[i + k] + 0.12f * rnd()));
    }
    Seq one(1, noisy);
    struct C { const char* id; Params p; };
    const C cs[] = {{"de.magix:DenoisingNLM", {{"Luminance", {6}}, {"Color", {6}}}}, {"com.sonycreativesoftware:smart_noise_reduction", {{"Noise", {5000}}}}};
    for (const C& c : cs) {
        const Def* d = findDef(c.id);
        if (!d) continue;
        const Buf out = runSeq(*d, c.p, {}, one, 0, 1.0, nullptr);
        const double a = psnr(noisy, clean), b = psnr(out, clean);
        check(b > a + 0.5, std::string(c.id) + ": noise is reduced", strfmt("PSNR %.2f -> %.2f dB", a, b));
        const Buf cl = runSeq(*d, c.p, {}, Seq(1, clean), 0, 1.0, nullptr);
        check(psnr(cl, clean) > 28.0, std::string(c.id) + ": a clean picture is kept", strfmt("PSNR %.2f dB", psnr(cl, clean)));
    }
}

static void aiTests()
{
    // grey picture: sky-like blue-white top, grass-like mid-grey bottom
    Buf g;
    g.resize(160, 90);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            const float v = y < 40 ? 0.8f - 0.2f * y / 40.f : 0.35f + 0.1f * std::sin(x * 0.7f) * std::sin(y * 0.9f);
            float* p = g.at(x, y);
            p[0] = p[1] = p[2] = v;
            p[3] = 1.f;
        }
    if (const Def* d = findDef("de.magix:Colorization")) {
        const Buf out = runSeq(*d, {}, {}, Seq(1, g), 0, 1.0, nullptr);
        double chroma = 0;
        for (size_t i = 0; i < out.px.size(); i += 4) chroma += std::fabs(out.px[i] - out.px[i + 2]) + std::fabs(out.px[i + 1] - out.px[i + 2]);
        chroma /= (double)(out.px.size() / 4);
        const float* sky = out.at(80, 8);
        const float* grass = out.at(80, 75);
        check(chroma > 0.01, "Colorization: a grey picture gets colour", strfmt("mean chroma %.4f", chroma));
        check(sky[2] > sky[0], "Colorization: the sky is bluer than red", strfmt("%.2f %.2f %.2f", sky[0], sky[1], sky[2]));
        check(grass[1] > grass[2], "Colorization: the foliage is greener than blue", strfmt("%.2f %.2f %.2f", grass[0], grass[1], grass[2]));
    }
    if (const Def* d = findDef("de.magix:StyleTransfer")) {
        const Buf pic = picture(160, 90, 0);
        std::vector<uint64_t> seen;
        bool allChange = true, finite = true;
        for (int s = 0; s < 17; ++s) {
            const Buf out = runSeq(*d, {{"Style", {(double)s}}}, {}, Seq(1, pic), 0, 1.0, nullptr);
            if (compare(out, pic).mean < 0.5) allChange = false;
            for (float v : out.px) if (!(v == v)) finite = false;
            seen.push_back(hashBuf(out));
        }
        std::sort(seen.begin(), seen.end());
        const bool distinct = std::unique(seen.begin(), seen.end()) == seen.end();
        check(allChange && finite && distinct, "Style Transfer: each of the 17 styles changes the picture, differently");
    }
}

static void motionTrackerTest()
{
    const Def* d = findDef("de.magix:MotionTracker");
    if (!d) return;
    const Buf pic = picture(160, 90, 0);
    const Buf out = runSeq(*d, {}, {}, Seq(1, pic), 0, 1.0, nullptr);
    check(compare(out, pic).mx == 0, "Motion Tracker: the picture passes through unchanged");
}

static void savePPM(const Buf& b, const char* path)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", b.w, b.h);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x)
            for (int k = 0; k < 3; ++k) std::fputc((int)std::lround(std::min(1.f, std::max(0.f, b.at(x, y)[k])) * 255.f), f);
    std::fclose(f);
}

// ------------------------------------------------------------------ 3. scale
static void scaleTests()
{
    const Seq big = shaky(640, 360, 12);
    const Seq half = reduceSeq(big, 2);
    struct C { const char* label; const char* id; Params p; double bound; };
    const C cs[] = {{"stabilize", "de.magix:Stabilize", {}, 4.0},
                    {"stabilize video", "com.vegascreativesoftware:Stabilize", {}, 4.0},
                    {"flicker", "de.magix:FlickerReducer", {}, 1.0},
                    {"denoise", "de.magix:DenoisingNLM", {{"Luminance", {5}}, {"Color", {5}}, {"Amount", {40}}}, 2.0},
                    {"smart noise", "com.sonycreativesoftware:smart_noise_reduction", {{"Noise", {6000}}}, 2.0},
                    {"colorization", "de.magix:Colorization", {}, 2.5},
                    {"style", "de.magix:StyleTransfer", {{"Style", {7}}}, 4.0}};
    for (const C& c : cs) {
        const Def* d = findDef(c.id);
        if (!d) continue;
        static int k1, k2;
        const int t = 7;
        Seq a = d->temporal ? renderAll(*d, c.p, {}, big, 1.0, &k1) : Seq(1, runSeq(*d, c.p, {}, big, t, 1.0, nullptr));
        Seq b = d->temporal ? renderAll(*d, c.p, {}, half, 0.5, &k2) : Seq(1, runSeq(*d, c.p, {}, half, t, 0.5, nullptr));
        const size_t i = d->temporal ? (size_t)t : 0;
        if (std::getenv("DUMP") && d->temporal && std::string(c.label) == "stabilize") { savePPM(reduce(a[3], 2), "/tmp/dbg_full.ppm"); savePPM(b[3], "/tmp/dbg_half.ppm"); savePPM(half[3], "/tmp/dbg_in.ppm"); }
        if (std::getenv("DBG") && d->temporal) for (size_t q = 0; q < a.size(); ++q) { int sx, sy; shiftBetween(reduce(a[q], 2), b[q], 6, sx, sy); std::printf("DBG   %s t%zu shake %d %d  residual %d %d  mean %.2f\n", c.label, q, shakeX((int)q), shakeY((int)q), sx, sy, compare(b[q], reduce(a[q], 2)).mean); }
        const Diff dd = compare(b[i], reduce(a[i], 2));
        if (std::getenv("DBG")) { int sx, sy; Buf ra = reduce(a[i], 2); shiftBetween(ra, b[i], 6, sx, sy); std::printf("DBG %s shift %d %d mean luma %.3f %.3f jit %.2f %.2f\n", c.label, sx, sy, meanLuma(ra), meanLuma(b[i]), jitter(a), jitter(b)); }
        check(dd.mean < c.bound, std::string(c.label) + ": half size is close to the full size averaged down", strfmt("mean %.3f largest %.1f levels", dd.mean, dd.mx));
    }
}

// ------------------------------------------------------------------ 4. threads
static void threadTests()
{
    const Seq in = shaky(256, 144, 10);
    for (const char* id : kIds) {
        const Def* d = findDef(id);
        if (!d) continue;
        std::vector<char> keys(4);
        const int t = 6;
        auto one = [&](const void* key) { return hashBuf(runSeq(*d, {}, {}, in, t, 1.0, key)); };
        const uint64_t want = one(&keys[0]);
        if (d->destroy) d->destroy(&keys[0]);
        std::vector<uint64_t> got(4, 0);
        std::vector<std::thread> th;
        for (int i = 0; i < 4; ++i) th.emplace_back([&, i] { got[(size_t)i] = one(&keys[(size_t)i]); });
        for (auto& x : th) x.join();
        for (int i = 0; i < 4; ++i) if (d->destroy) d->destroy(&keys[(size_t)i]);
        bool same = true;
        for (uint64_t h : got) if (h != want) same = false;
        check(same, std::string(id) + ": four threads, each its own instance, get what one thread gets");
        // the same instance rendered from two threads at once
        std::vector<uint64_t> got2(3, 0);
        std::vector<std::thread> t2;
        char shared = 0;
        for (int i = 0; i < 3; ++i) t2.emplace_back([&, i] { got2[(size_t)i] = one(&shared); });
        for (auto& x : t2) x.join();
        if (d->destroy) d->destroy(&shared);
        same = true;
        for (uint64_t h : got2) if (h != want) same = false;
        check(same, std::string(id) + ": three threads on one instance get the same picture");
    }
}

// ------------------------------------------------------------------ 5. engine and .veg
static void engineChecks()
{
    Rig rig;
    for (const char* id : kIds) {
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        e.fx.push_back(slot(p, id, Json::object()));
        p.tracks[0].events.push_back(e);
        rig.fresh();
        ImagePtr full = rig.render(p, 1.0, 1.0);
        const std::string why = rig.fx.error(p.tracks[0].events[0].fx[0].id);
        check(full && why.empty() && rig.fx.video(p.tracks[0].events[0].fx[0].id) != nullptr, std::string(id) + ": loads and renders through the OpenFX host", why);
        ImagePtr half = rig.render(p, 1.0, 0.5);
        check(half != nullptr, std::string(id) + ": renders at half size");
        ofx::Plugin* pl = findVideoPlugin(id, nullptr);
        check(pl && pl->info.multiResolution, std::string(id) + ": described as multi-resolution");
        // a .veg write and read keeps the values
        rig.fx.captureStates(p);
        std::vector<std::string> warn;
        const std::string bytes = writeVegasData(p, "/tmp/analysis_rt.veg", &warn);
        const std::string tmp = "/tmp/analysis_roundtrip.veg";
        Project q;
        std::string err;
        bool ok = !bytes.empty() && writeFile(tmp, bytes) && loadProjectFile(tmp, q, &err);
        bool found = false;
        if (ok) q.forEachFx([&](FxSlot& s) { if (s.pluginId == id) found = true; });
        check(ok && found, std::string(id) + ": survives a .veg write and read", err);
        rig.fx.releaseExcept({});
    }
}

// The buttons through the host: pressing one is an OpenFX "instance changed" the built-in answers (Analyze runs, Clear drops).
static void buttonTests()
{
    Rig rig;
    struct B { const char* id; const char* press; const char* clear; };
    const B bs[] = {{"de.magix:Stabilize", "AnalyzeMotion", "CLearMotion"}, {"de.magix:Stabilize", "AnalyzeMotionBasic", "CLearMotionBasic"},
                    {"com.vegascreativesoftware:Stabilize", "AnalyzeButton", nullptr}, {"de.magix:FlickerReducer", "Calc", "Clear"}};
    for (const B& b : bs) {
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        e.fx.push_back(slot(p, b.id, Json::object()));
        p.tracks[0].events.push_back(e);
        rig.fresh();
        ImagePtr full = rig.render(p, 1.0, 1.0);
        ofx::Instance* inst = rig.fx.video(p.tracks[0].events[0].fx[0].id);
        check(full && inst, std::string(b.id) + " " + b.press + ": instance to press the button on");
        if (!inst) continue;
        ofx::Param* pb = inst->params.find(b.press);
        check(pb && pb->type == "OfxParamTypePushButton", std::string(b.id) + ": " + b.press + " is a push button of the instance");
        if (pb) inst->userPushButton(pb, 0.0);
        ofx::Param* has = inst->params.find("HasMotionData");
        if (has && pb) check(has->value[0] == 1.0, std::string(b.id) + ": " + b.press + " analyses (HasMotionData set)");
        if (b.clear) {
            ofx::Param* cb = inst->params.find(b.clear);
            if (cb) inst->userPushButton(cb, 0.0);
            if (has && cb) check(has->value[0] == 0.0, std::string(b.id) + ": " + b.clear + " clears it");
        }
        ImagePtr again = rig.render(p, 1.0, 1.0);
        check(again != nullptr, std::string(b.id) + ": still renders after the buttons");
        rig.fx.releaseExcept({});
    }
}

int main()
{
    if (std::getenv("DUMPDEFS")) {
        for (const char* id : kIds) {
            const Def* d = findDef(id);
            if (!d) continue;
            for (const PDef& p : d->params) {
                std::printf("DEF\t%s\t%d\t%s\t%s\t%s\t", id, (int)p.type, p.name.c_str(), p.label.c_str(), p.secret ? "secret" : "");
                for (size_t i = 0; i < p.choices.size(); ++i) std::printf("%s%s", i ? "|" : "", p.choices[i].c_str());
                std::printf("\t");
                for (double v : p.def) std::printf("%g ", v);
                std::printf("\t%g %g\n", p.min, p.max);
            }
        }
        return 0;
    }
    ofx::setTextRasterizer(blockRasterizer);
    defTests();
    stabilizeTests();
    flickerTests();
    denoiseTests();
    aiTests();
    motionTrackerTest();
    scaleTests();
    threadTests();
    engineChecks();
    buttonTests();
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
