// Tests of the project model, the compositor, the mixer and the export, with made-up media
// (every picture says which frame it is; every sound is a steady tone) and real plug-ins.
//
//   enginetest <testfx.ofx> [SlopFX.ofx] [a VST2 effect] [folder to write a test render into]
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const double kPi = 3.14159265358979323846;

// Media that is made up: "synth:<tag>" is a 10-second clip whose every pixel is (blue = frame number, green = tag,
// red = 200) and whose sound is a tone of 220 Hz x tag at half volume.
class SynthReader : public MediaReader {
public:
    MediaInfo mi;
    int tag = 1;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        if (!mi.hasVideo) return nullptr;
        int frame = (int)std::floor(seconds * mi.fps + 1e-6);
        frame = std::max(0, std::min(frame, (int)std::lround(mi.duration * mi.fps) - 1));
        ImagePtr img = Image::make(maxW, maxH, PixelDepth::Byte, false);
        for (int y = 0; y < img->h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < img->w; ++x, d += 4) { d[0] = (uint8_t)frame; d[1] = (uint8_t)tag; d[2] = 200; d[3] = 255; }
        }
        return img;
    }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        if (!mi.hasAudio) return false;
        const int64_t end = (int64_t)std::llround(mi.duration * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const float v = (n < 0 || n >= end) ? 0.f : 0.5f * (float)std::sin(2.0 * kPi * 220.0 * tag * (double)n / sampleRate);
            left[k] = v;
            right[k] = v;
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> synthOpen(const std::string& path, std::string* error)
{
    if (path.compare(0, 6, "synth:") != 0) return openMedia(path, error);
    std::unique_ptr<SynthReader> r(new SynthReader());
    r->tag = std::atoi(path.c_str() + 6);
    r->mi.path = path;
    r->mi.hasVideo = r->mi.hasAudio = true;
    r->mi.width = 320;
    r->mi.height = 180;
    r->mi.fps = 30;
    r->mi.duration = 10;
    r->mi.sampleRate = 48000;
    r->mi.channels = 2;
    return std::unique_ptr<MediaReader>(r.release());
}

static uint64_t addMedia(Project& p, int tag)
{
    MediaItem m;
    m.id = p.newId();
    m.path = strfmt("synth:%d", tag);
    m.name = m.path;
    std::string e;
    m.info = synthOpen(m.path, &e)->info();
    p.media.push_back(m);
    return m.id;
}

static Track& addTrack(Project& p, TrackType type)
{
    Track t;
    t.id = p.newId();
    t.type = type;
    p.tracks.push_back(t);
    return p.tracks.back();
}

static Event& addEvent(Project& p, Track& t, uint64_t media, double start, double length, double offset = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = offset;
    t.events.push_back(e);
    t.sortEvents();
    for (auto& x : t.events) if (x.id == e.id) return x;
    return t.events.back();
}

static FxSlot ofxSlot(Project& p, const std::string& id)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = id;
    s.name = id;
    return s;
}

struct Px { int b, g, r, a; };
static Px pixel(const ImagePtr& img, int x = 10, int y = 10)
{
    if (!img) return {-1, -1, -1, -1};
    const uint8_t* d = img->row(y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d a=%d", p.b, p.g, p.r, p.a); }
static bool near(int a, int b, int tol = 2) { return std::abs(a - b) <= tol; }

// The strength of one frequency in a block of sound (amplitude of the best-fitting sine).
static double tone(const std::vector<float>& s, double freq, int rate)
{
    double re = 0, im = 0;
    for (size_t k = 0; k < s.size(); ++k) {
        re += s[k] * std::cos(2.0 * kPi * freq * (double)k / rate);
        im += s[k] * std::sin(2.0 * kPi * freq * (double)k / rate);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double)s.size();
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: enginetest testfx.ofx [SlopFX.ofx] [vst2 effect] [output folder]\n"); return 2; }
    const std::string testfx = argv[1], slopfx = argc > 2 ? argv[2] : "", vst = argc > 3 ? argv[3] : "", outDir = argc > 4 ? argv[4] : "";
    std::string err;
    check(ofx::Host::get().loadBinary(testfx, &err) != nullptr, "the test effects load", err);
    if (!slopfx.empty()) check(ofx::Host::get().loadBinary(slopfx, &err) != nullptr, "SlopFX loads", err);

    // ---------------------------------------------------------------- the project as a value
    {
        Project p;
        p.settings.width = 320;
        p.settings.height = 180;
        const uint64_t m = addMedia(p, 3);
        Track& v = addTrack(p, TrackType::Video);
        v.name = "Video";
        Event& e = addEvent(p, v, m, 1.5, 4.0, 0.25);
        e.fadeIn = 0.5;
        e.rate = 2.0;
        e.fx.push_back(ofxSlot(p, "reno.test.invert"));
        e.transition = ofxSlot(p, "reno.test.transition");
        Track& a = addTrack(p, TrackType::Audio);
        a.pan = -0.5;
        addEvent(p, a, m, 0, 2);
        p.videoOut.push_back(ofxSlot(p, "reno.test.temporal"));
        const std::string one = p.toJson().dump(1);
        Project q;
        check(q.fromJson(p.toJson(), &err), "a project reads back from its own JSON", err);
        check(q.toJson().dump(1) == one, "and writes the same JSON again");
        check(q.nextId >= p.nextId, "ids keep counting from where they were");
        check(std::fabs(q.length() - 5.5) < 1e-9, "length is where the last event ends", strfmt("%g", q.length()));
        Json bad;
        Json::parse("{\"x\":1}", bad);
        check(!q.fromJson(bad, &err), "something that is not a project is refused");
        check(p.frameAt(1.0) == 30 && p.frameAt(0.999999999) == 30 && p.frameAt(0.98) == 29, "times map to frames");
        check(std::fabs(eventMediaTime(e, p.findMedia(m), 2.5) - 2.25) < 1e-9, "media time follows offset and rate", strfmt("%g", eventMediaTime(e, p.findMedia(m), 2.5)));
        Event lp = e;
        lp.rate = 1;
        lp.offset = 9;
        lp.start = 0;
        check(std::fabs(eventMediaTime(lp, p.findMedia(m), 2.0) - 1.0) < 1e-9, "media time wraps round when the event loops");
        lp.loop = false;
        check(std::fabs(eventMediaTime(lp, p.findMedia(m), 2.0) - 10.0) < 1e-9, "and holds at the end when it does not");
    }

    FxRuntime fx;
    MediaPool pool;
    pool.setOpener(synthOpen);
    VideoEngine video(pool, fx);
    AudioEngine audio(pool, fx);
    FrameOptions fo;

    // ---------------------------------------------------------------- pictures
    {
        Project p;
        p.settings.width = 320;
        p.settings.height = 180;
        ImagePtr img = video.render(p, 0.0, fo);
        Px x = pixel(img);
        check(img && img->w == 320 && img->h == 180 && x.b == 0 && x.g == 0 && x.r == 0 && x.a == 255, "an empty project is opaque black", pxs(x));

        const uint64_t m1 = addMedia(p, 1), m2 = addMedia(p, 2);
        Track& t1 = addTrack(p, TrackType::Video);
        const uint64_t t1id = t1.id;
        const uint64_t e1 = addEvent(p, t1, m1, 1.0, 3.0, 0.5).id;
        fx.prepare(p);
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 45 && x.g == 1 && x.r == 200 && x.a == 255, "an event shows the right frame of its media (1 s in, 0.5 s offset = frame 45)", pxs(x));
        x = pixel(video.render(p, 2.0 + 0.02, fo));
        check(x.b == 45, "a time inside a frame shows that frame", pxs(x));
        x = pixel(video.render(p, 0.5, fo));
        check(x.b == 0 && x.g == 0 && x.r == 0, "before the event there is black", pxs(x));
        x = pixel(video.render(p, 4.0, fo));
        check(x.g == 0, "the event's end is not included", pxs(x));
        p.findEvent(e1)->rate = 2.0;
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 75, "at double speed the media runs twice as fast (frame 75)", pxs(x));
        p.findEvent(e1)->rate = 1.0;

        p.findEvent(e1)->fadeIn = 1.0;
        x = pixel(video.render(p, 1.5, fo));
        check(near(x.r, 100, 3) && x.a == 255, "half way through a fade-in the picture is at half strength", pxs(x));
        p.findEvent(e1)->fadeIn = 0;
        p.findEvent(e1)->gain = 0.25;
        x = pixel(video.render(p, 2.0, fo));
        check(near(x.r, 50, 3), "an event's opacity scales it", pxs(x));
        p.findEvent(e1)->gain = 1.0;

        // a second event overlapping the first by one second: a cross-fade
        Track* tp = p.findTrack(t1id);
        const uint64_t e2 = addEvent(p, *tp, m2, 3.0, 3.0).id;
        x = pixel(video.render(p, 3.5, fo));
        check(near(x.g, 1 * 0.5 + 2 * 0.5 + 0.5, 1) && near(x.r, 200, 3), "overlapping events cross-fade (half and half in the middle)", pxs(x));
        x = pixel(video.render(p, 3.0 + 1.0 / 30, fo));
        check(near(x.r, 200, 4), "and the cross-fade keeps full strength throughout", pxs(x));
        x = pixel(video.render(p, 4.5, fo));
        check(x.g == 2 && x.b == 45, "after the overlap only the later event shows", pxs(x));

        // the same overlap through an OpenFX transition
        p.findEvent(e2)->transition = ofxSlot(p, "reno.test.transition");
        fx.prepare(p);
        check(fx.video(p.findEvent(e2)->transition.id) != nullptr, "an OpenFX transition can be made", fx.error(p.findEvent(e2)->transition.id));
        x = pixel(video.render(p, 3.25, fo));
        check(near(x.r, 200, 3) && x.a == 255, "the transition renders the overlap", pxs(x));
        Px q1 = pixel(video.render(p, 3.0 + 3.0 / 30, fo)), q3 = pixel(video.render(p, 3.0 + 27.0 / 30, fo));
        check(q1.b != q3.b || q1.g != q3.g, "and moves from the first event to the second", pxs(q1) + " / " + pxs(q3));
        p.findEvent(e2)->transition = FxSlot();

        // an effect on an event
        p.findEvent(e1)->fx.push_back(ofxSlot(p, "reno.test.invert"));
        const uint64_t inv = p.findEvent(e1)->fx[0].id;
        fx.prepare(p);
        check(fx.video(inv) != nullptr, "an effect on an event can be made", fx.error(inv));
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 255 - 45 && x.g == 254 && x.r == 55, "an effect on an event changes that event (inverted)", pxs(x));
        x = pixel(video.render(p, 4.5, fo));
        check(x.g == 2 && x.r == 200, "and not the next event", pxs(x));
        FrameOptions noFx;
        noFx.skipEffects = true;
        x = pixel(video.render(p, 2.0, noFx));
        check(x.b == 45 && x.r == 200, "effects can all be left out", pxs(x));
        p.findEvent(e1)->fx[0].bypass = true;
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 45, "a bypassed effect does nothing", pxs(x));
        p.findEvent(e1)->fx[0].bypass = false;

        // a temporal effect on the event: averages frames t-1, t, t+1 of the event (still frame-numbered)
        p.findEvent(e1)->fx.clear();
        p.findEvent(e1)->fx.push_back(ofxSlot(p, "reno.test.temporal"));
        fx.prepare(p);
        x = pixel(video.render(p, 2.0, fo));
        check(near(x.b, 45, 1) && x.g == 1, "an effect that looks at other frames gets the frames either side", pxs(x));
        x = pixel(video.render(p, 1.0, fo));
        check(near(x.b, (15 + 15 + 16) / 3, 1) || near(x.b, (14 + 15 + 16) / 3, 1), "and at the event's first frame gets what comes before it in the media", pxs(x));
        p.findEvent(e1)->fx.clear();

        // tracks: a second track above, half see-through
        {
            Track t;
            t.id = p.newId();
            t.type = TrackType::Video;
            p.tracks.insert(p.tracks.begin(), t);                  // track 0 = on top
        }
        const uint64_t topId = p.tracks[0].id;
        addEvent(p, p.tracks[0], m2, 0.0, 10.0);
        x = pixel(video.render(p, 2.0, fo));
        check(x.g == 2 && x.b == 60, "the top track covers the one below", pxs(x));
        p.findTrack(topId)->level = 0.5;
        x = pixel(video.render(p, 2.0, fo));
        check(near(x.b, (60 + 45) / 2, 2) && near(x.r, 200, 3), "a half see-through track mixes with the one below", pxs(x));
        p.findTrack(topId)->mute = true;
        x = pixel(video.render(p, 2.0, fo));
        check(x.g == 1 && x.b == 45, "a muted track is left out", pxs(x));
        p.findTrack(topId)->mute = false;
        p.findTrack(topId)->level = 1.0;
        p.findTrack(t1id)->solo = true;
        x = pixel(video.render(p, 2.0, fo));
        check(x.g == 1, "a soloed track is the only one shown", pxs(x));
        p.findTrack(t1id)->solo = false;

        // effects on a track and on the whole picture
        p.findTrack(topId)->fx.push_back(ofxSlot(p, "reno.test.invert"));
        fx.prepare(p);
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 255 - 60 && x.g == 253, "an effect on a track changes that track", pxs(x));
        p.findTrack(topId)->fx.clear();
        p.findTrack(topId)->mute = true;
        p.videoOut.push_back(ofxSlot(p, "reno.test.invert"));
        fx.prepare(p);
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 255 - 45 && x.r == 55, "an effect on the output changes everything", pxs(x));
        x = pixel(video.render(p, 0.5, fo));
        check(x.a == 255, "and the result is still opaque where nothing plays", pxs(x));
        p.videoOut.clear();

        // an effect that fails to render must not lose the picture
        p.findEvent(e1)->fx.push_back(ofxSlot(p, "reno.test.failrender"));
        const uint64_t failing = p.findEvent(e1)->fx[0].id;
        fx.prepare(p);
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 45 && x.g == 1, "an effect that fails passes its input through", pxs(x));
        check(!fx.error(failing).empty(), "and the failure is remembered for the user", fx.error(failing));
        p.findEvent(e1)->fx.clear();
        p.findEvent(e1)->fx.push_back(ofxSlot(p, "no.such.effect"));
        fx.prepare(p);
        x = pixel(video.render(p, 2.0, fo));
        check(x.b == 45 && !fx.error(p.findEvent(e1)->fx[0].id).empty(), "an effect that is not installed is skipped and reported", fx.error(p.findEvent(e1)->fx[0].id));
        p.findEvent(e1)->fx.clear();

        // generated media
        MediaItem g;
        g.id = p.newId();
        g.name = "Generated";
        g.generated = true;
        g.generator = ofxSlot(p, "reno.test.generator");
        g.info.hasVideo = true;
        p.media.push_back(g);
        const uint64_t ge = addEvent(p, *p.findTrack(t1id), g.id, 7.0, 2.0).id;
        fx.prepare(p);
        ImagePtr gi = video.render(p, 7.5, fo);
        x = pixel(gi, 100, 50);
        check(x.r == 15 && x.g == 50 && x.b == 100, "a generator makes pictures (frame 15 of it at 0.5 s)", pxs(x));
        (void)ge;

        // settings survive being saved through the runtime
        p.findEvent(e1)->fx.push_back(ofxSlot(p, "reno.test.params"));
        const uint64_t ps = p.findEvent(e1)->fx[0].id;
        fx.prepare(p);
        ofx::Instance* inst = fx.video(ps);
        check(inst != nullptr, "the every-kind-of-setting effect can be made", fx.error(ps));
        if (inst) {
            ofx::Param* dbl = nullptr;
            for (const auto& prm : inst->params.params) if (prm->kind == ofx::ParamKind::Double && !dbl) dbl = prm.get();
            if (dbl) {
                const uint64_t before = fx.videoRevision();
                inst->userSetValue(dbl, {0.375}, 0.0);
                check(fx.videoRevision() != before, "changing a setting changes the picture stamp");
                fx.captureStates(p);
                Project q;
                q.fromJson(p.toJson());
                FxRuntime fx2;
                fx2.prepare(q);
                ofx::Instance* inst2 = fx2.video(ps);
                ofx::Param* d2 = inst2 ? inst2->params.find(dbl->name) : nullptr;
                check(d2 && std::fabs(d2->value[0] - 0.375) < 1e-9, "a setting comes back after save and load", d2 ? strfmt("%g", d2->value[0]) : "no instance");
            }
        }
        std::set<uint64_t> keep;
        fx.releaseExcept(keep);
        check(fx.video(ps) == nullptr, "instances that are no longer wanted are let go");
    }

    // ---------------------------------------------------------------- SlopFX through the engine
    if (!slopfx.empty()) {
        Project p;
        p.settings.width = 640;
        p.settings.height = 360;
        const uint64_t m = addMedia(p, 1);
        Track& t = addTrack(p, TrackType::Video);
        Event& e = addEvent(p, t, m, 0, 5);
        const uint64_t eid = e.id;
        int ok = 0, total = 0;
        std::string bad;
        for (ofx::Plugin* pl : ofx::Host::get().allPlugins()) {
            if (pl->info.id.compare(0, 4, "com.") != 0) continue;
            if (pl->binary && pl->binary->builtin) continue;       // (Reno's own: builtins_test)
            ++total;
            p.findEvent(eid)->fx.clear();
            p.findEvent(eid)->fx.push_back(ofxSlot(p, pl->info.id));
            fx.prepare(p);
            const uint64_t sid = p.findEvent(eid)->fx[0].id;
            bool good = fx.video(sid) != nullptr;
            for (int f = 0; f < 4 && good; ++f) {
                ImagePtr img = video.render(p, 1.0 + f / 30.0, fo);
                good = img && img->w == 640 && pixel(img).a == 255 && fx.error(sid).empty();
            }
            if (good) ++ok; else bad += " " + pl->info.id + "(" + fx.error(sid) + ")";
        }
        check(total > 0 && ok == total, strfmt("all %d SlopFX effects render on an event through the engine", total), bad);
        fx.clear();
    }

    // ---------------------------------------------------------------- sound
    {
        Project p;
        const int rate = p.settings.sampleRate;
        const uint64_t m1 = addMedia(p, 1), m2 = addMedia(p, 2);       // 220 Hz and 440 Hz
        Track& a = addTrack(p, TrackType::Audio);
        const uint64_t aid = a.id;
        const uint64_t e1 = addEvent(p, a, m1, 1.0, 3.0).id;
        std::vector<float> l(4800), r(4800);
        auto block = [&](double t) { audio.render(p, (int64_t)std::llround(t * rate), (int)l.size(), l.data(), r.data(), true); };
        block(0.5);
        check(audio.peakLeft == 0 && audio.peakRight == 0, "silence before the event");
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.5) < 0.01 && std::fabs(tone(r, 220, rate) - 0.5) < 0.01, "an event plays its sound", strfmt("%.3f", tone(l, 220, rate)));
        // reading on in small blocks must join up: one long read against many short ones
        std::vector<float> longL(9600), longR(9600), shortL(9600), shortR(9600);
        audio.render(p, rate * 2, 9600, longL.data(), longR.data(), true);
        for (int at = 0; at < 9600; at += 441) audio.render(p, rate * 2 + at, std::min(441, 9600 - at), shortL.data() + at, shortR.data() + at, true);
        double worst = 0;
        for (int k = 0; k < 9600; ++k) worst = std::max(worst, (double)std::fabs(longL[k] - shortL[k]));
        check(worst < 1e-6, "blocks of any size join up exactly", strfmt("%g", worst));
        // the event's edges land on the right sample
        std::vector<float> el(rate), er(rate);
        audio.render(p, rate / 2, rate, el.data(), er.data(), true);
        int firstSound = -1;
        for (int k = 0; k < rate; ++k) if (std::fabs(el[k]) > 1e-6f) { firstSound = k; break; }
        check(std::abs(firstSound - rate / 2) <= 1, "the event starts on the right sample", strfmt("%d", firstSound));

        p.findEvent(e1)->rate = 2.0;
        block(2.0);
        check(std::fabs(tone(l, 440, rate) - 0.5) < 0.02 && tone(l, 220, rate) < 0.02, "at double speed the pitch doubles", strfmt("%.3f at 440, %.3f at 220", tone(l, 440, rate), tone(l, 220, rate)));
        p.findEvent(e1)->rate = 1.0;
        p.findEvent(e1)->gain = 0.5;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.25) < 0.01, "an event's volume scales it");
        p.findEvent(e1)->gain = 1.0;
        p.findEvent(e1)->fadeIn = 2.0;
        block(1.95);                                                   // centred on 2.0: half way up the fade
        check(std::fabs(tone(l, 220, rate) - 0.25) < 0.02, "a fade-in ramps the sound", strfmt("%.3f", tone(l, 220, rate)));
        p.findEvent(e1)->fadeIn = 0;

        p.findTrack(aid)->pan = 1.0;
        block(2.0);
        check(tone(l, 220, rate) < 0.001 && std::fabs(tone(r, 220, rate) - 0.5) < 0.01, "pan right silences the left");
        p.findTrack(aid)->pan = 0;
        p.findTrack(aid)->level = 0.5;
        p.masterVolume = 0.5;
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.125) < 0.01, "track volume and master volume both apply");
        p.findTrack(aid)->level = 1.0;
        p.masterVolume = 1.0;

        // looping: an event longer than its media carries on from the start
        Event& lp = addEvent(p, *p.findTrack(aid), m2, 20.0, 15.0);
        const uint64_t lpid = lp.id;
        block(31.0);
        check(std::fabs(tone(l, 440, rate) - 0.5) < 0.01, "a looping event plays past the end of its media");
        p.findEvent(lpid)->loop = false;
        block(31.0);
        check(audio.peakLeft < 1e-6, "an event that does not loop is silent past the end");

        // cross-fade: two tones overlapping by a second
        addEvent(p, *p.findTrack(aid), m2, 3.0, 3.0);
        block(3.45);                                                   // centred on 3.5
        check(std::fabs(tone(l, 220, rate) - 0.25) < 0.03 && std::fabs(tone(l, 440, rate) - 0.25) < 0.03, "overlapping events cross-fade",
              strfmt("%.3f and %.3f", tone(l, 220, rate), tone(l, 440, rate)));

        // a second track adds to the first; mute and solo
        Track& b = addTrack(p, TrackType::Audio);
        const uint64_t bid = b.id;
        addEvent(p, b, m2, 0.0, 3.0);
        block(2.0);
        check(std::fabs(tone(l, 220, rate) - 0.5) < 0.01 && std::fabs(tone(l, 440, rate) - 0.5) < 0.01, "tracks add together");
        p.findTrack(bid)->mute = true;
        block(2.0);
        check(tone(l, 440, rate) < 0.001, "a muted track is silent");
        p.findTrack(bid)->mute = false;
        p.findTrack(bid)->solo = true;
        block(2.0);
        check(tone(l, 220, rate) < 0.001 && tone(l, 440, rate) > 0.49, "a soloed track is the only one heard");
        p.findTrack(bid)->solo = false;

        // a VST effect on a track
        if (!vst.empty()) {
            std::vector<AudioPluginDesc> d = describeAudioPluginFile(vst, &err);
            check(!d.empty(), "the VST effect is recognised", err);
            if (!d.empty()) {
                FxSlot s;
                s.id = p.newId();
                s.kind = "audio";
                s.audio = d[0];
                s.name = d[0].name;
                p.findTrack(bid)->fx.push_back(s);
                p.findTrack(bid)->solo = true;
                block(2.0);
                const std::vector<float> dry = l;
                fx.prepare(p);
                check(fx.audio(s.id) != nullptr, "a VST effect on a track can be made", fx.error(s.id));
                for (int k = 0; k < 5; ++k) block(2.0 + k * 0.1);
                double diff = 0;
                block(2.0);
                for (size_t k = 0; k < l.size(); ++k) diff = std::max(diff, (double)std::fabs(l[k] - dry[k]));
                check(diff > 0.001 && audio.peakLeft > 0.001, "and changes the track's sound", strfmt("difference %.4f", diff));
                p.findTrack(bid)->fx[0].bypass = true;
                block(2.0);
                diff = 0;
                for (size_t k = 0; k < l.size(); ++k) diff = std::max(diff, (double)std::fabs(l[k] - dry[k]));
                check(diff < 1e-6, "a bypassed audio effect does nothing");
                fx.captureStates(p);
                check(p.findTrack(bid)->fx[0].state.isString() && !p.findTrack(bid)->fx[0].state.asString().empty(), "its settings are captured for saving");
            }
        }
        fx.clear();
    }

    // ---------------------------------------------------------------- export, then read the file back
    if (!outDir.empty()) {
        Project p;
        p.settings.width = 320;
        p.settings.height = 180;
        const uint64_t m1 = addMedia(p, 1);
        Track& v = addTrack(p, TrackType::Video);
        addEvent(p, v, m1, 0.0, 3.0, 1.0);
        Track& a = addTrack(p, TrackType::Audio);
        addEvent(p, a, m1, 0.0, 3.0);
        EncodeSettings es;
        es.path = pathJoin(outDir, "enginetest.mp4");
        std::string desc;
        int calls = 0;
        bool ok = exportProject(p, fx, es, 0.0, 2.0, [&](const ExportProgress& ep) { ++calls; return ep.fraction <= 1.0; }, &err, synthOpen, &desc);
        check(ok, "a project renders to a file", err.empty() ? desc : err);
        check(calls == 60, "progress is reported once a frame", strfmt("%d", calls));
        if (ok) {
            std::unique_ptr<MediaReader> rd = openMedia(es.path, &err);
            check(rd != nullptr, "the file opens again", err);
            if (rd) {
                const MediaInfo& mi = rd->info();
                check(mi.width == 320 && mi.height == 180 && std::fabs(mi.fps - 30) < 0.01 && std::fabs(mi.duration - 2.0) < 0.1,
                      "it has the project's size, frame rate and length", strfmt("%dx%d %.3f fps %.3f s", mi.width, mi.height, mi.fps, mi.duration));
                Px x = pixel(rd->videoFrame(1.0, 320, 180));
                check(near(x.b, 60, 8) && near(x.g, 1, 8) && near(x.r, 200, 8), "the frame at 1 s is the frame that was rendered (media frame 60)", pxs(x));
                std::vector<float> l(9600), r(9600);
                rd->audio(48000, 9600, 48000, l.data(), r.data());
                check(std::fabs(tone(l, 220, 48000) - 0.5) < 0.05, "and the sound is the tone that was rendered", strfmt("%.3f", tone(l, 220, 48000)));
            }
        }
        bool stopped = exportProject(p, fx, es, 0.0, 2.0, [&](const ExportProgress& ep) { return ep.frame < 5; }, &err, synthOpen);
        check(!stopped && err == "stopped", "an export can be stopped part way", err);
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
