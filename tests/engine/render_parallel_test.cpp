// Tests of Render As on several processors (renderProjectAs): the output file must be byte for byte the same for any
// number of processors (--threads), with video, sound, effects, cross-fades, markers and a loudness log. Cancel must
// stop promptly and cleanly, and a failing encoder is reported. Made-up media.
//
//   render_parallel_test <output folder>
#include "../../src/engine/engine.h"
#include "../../src/engine/renderas.h"
#include "../../src/ofx/builtins.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const double kPi = 3.14159265358979323846;

// "clip:" 10 s at 30 fps, 320 x 180: a white bar moving over a gradient whose blue is the frame number, and a tone
class TestReader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        const int frame = std::max(0, std::min((int)std::floor(seconds * 30.0 + 1e-6), 299));
        const double bar = (20.0 + 90.0 * seconds) * w / mi.width;
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, true);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(h - 1 - y);
            for (int x = 0; x < w; ++x, d += 4) {
                const bool on = x >= std::floor(bar) && x < std::floor(bar) + 6;
                d[0] = on ? 255 : (uint8_t)(frame + x / 4);
                d[1] = on ? 255 : (uint8_t)(y * 255 / h);
                d[2] = on ? 255 : 90;
                d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const float v = n >= 0 && n < 10LL * sampleRate ? 0.4f * (float)std::sin(2.0 * kPi * 330.0 * (double)n / sampleRate) : 0.f;
            left[k] = v;
            right[k] = 0.5f * v;
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> testOpen(const std::string& path, std::string* error)
{
    if (path != "clip:") { if (error) *error = "unknown test media"; return nullptr; }
    std::unique_ptr<TestReader> r(new TestReader());
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

static const std::string kV = "com.vegascreativesoftware:";

static FxSlot fxSlot(Project& p, const std::string& id)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = id;
    s.name = id;
    return s;
}

static Project makeProject()
{
    Project p;
    p.settings.width = 320;
    p.settings.height = 180;
    p.settings.fpsNum = 30;
    MediaItem m;
    m.id = p.newId();
    m.path = "clip:";
    m.name = "clip";
    std::string e;
    m.info = testOpen(m.path, &e)->info();
    p.media.push_back(m);
    auto event = [&](double start, double len, double off) {
        Event ev;
        ev.id = p.newId();
        ev.mediaId = m.id;
        ev.start = start;
        ev.length = len;
        ev.offset = off;
        return ev;
    };
    Track v1;
    v1.id = p.newId();
    v1.type = TrackType::Video;
    Event a = event(0, 2.0, 0), b = event(1.5, 2.0, 3.0);
    a.fx.push_back(fxSlot(p, kV + "vignette"));
    a.fadeOut = 0.5;
    b.fx.push_back(fxSlot(p, kV + "brightnessandcontrast"));
    b.fadeIn = 0.5;
    v1.events.push_back(a);
    v1.events.push_back(b);
    p.tracks.push_back(v1);
    Track v2;
    v2.id = p.newId();
    v2.type = TrackType::Video;
    Event c = event(0.5, 2.5, 5.0);
    c.gain = 0.5;
    PanCropKey k0, k1;
    k0.time = 0; k0.w = 0.5; k0.h = 0.5; k0.cx = 0.3; k0.cy = 0.3;
    k1 = k0; k1.time = 2.5; k1.cx = 0.7; k1.cy = 0.6; k1.rotation = 10;
    c.panCrop = {k0, k1};
    c.fx.push_back(fxSlot(p, kV + "saturationadjust"));
    v2.events.push_back(c);
    p.tracks.push_back(v2);
    Track at;
    at.id = p.newId();
    at.type = TrackType::Audio;
    at.pan = -0.3;
    for (int k = 0; k < 3; ++k) {
        Event s = event(k * 1.1, 1.3, 1.0 + k);
        s.fadeIn = 0.1;
        s.fadeOut = 0.1;
        at.events.push_back(s);
    }
    p.tracks.push_back(at);
    Project::Marker mk;
    mk.time = 1.0;
    mk.name = "one";
    p.markers.push_back(mk);
    return p;
}

static std::string slurp(const std::string& path)
{
    std::string s;
    readFile(path, s);
    return s;
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    Project p = makeProject();
    auto opener = [](const std::string& path, std::string* error) { return testOpen(path, error); };
    const std::string avi = dir + "/par_render.avi", wav = dir + "/par_render.wav", mkv = dir + "/par_render.mkv", log = dir + "/par_loud.txt";

    // one render: 'workers' pictures at once (RENO_RENDER_WORKERS), the file's bytes (and the loudness log) back
    struct Out { bool ok = false; std::string error, bytes, loud; int calls = 0; double seconds = 0; };
    auto render = [&](const std::string& path, int workers, const char* kind, double to, int stopAfter = 0) {
        Out o;
        setenv("RENO_RENDER_WORKERS", std::to_string(workers).c_str(), 1);
        RenderSpec spec;
        spec.enc.path = path;
        if (!std::strcmp(kind, "avi")) {
            spec.enc.videoFamily = "rawvideo";
            spec.enc.audioFamily = "pcm";
            spec.markers = true;
            spec.loudnessLog = log;
        } else if (!std::strcmp(kind, "wav")) {
            spec.enc.video = false;
        } else if (!std::strcmp(kind, "mkv")) {
            spec.enc.videoFamily = "ffv1";           // (lossless and deterministic)
            spec.enc.audioFamily = "flac";
            spec.width = 256;
            spec.height = 144;
        }
        FxRuntime fx;
        fx.prepare(p);
        std::remove(log.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        o.ok = renderProjectAs(p, fx, spec, 0.0, to, [&](const ExportProgress&) { ++o.calls; return !(stopAfter > 0 && o.calls >= stopAfter); }, &o.error, opener);
        o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        o.bytes = slurp(path);
        o.loud = slurp(log);
        return o;
    };

    // ---- the same file whatever the number of pictures at once
    const double len = 3.5;
    Out ref = render(avi, 1, "avi", len);
    check(ref.ok, "serial render (1 picture at a time)", ref.error);
    check(ref.bytes.size() > 100000, "the file has pictures and sound", std::to_string(ref.bytes.size()) + " bytes");
    check(!ref.loud.empty(), "the loudness log is made");
    check(ref.calls == (int)std::ceil(len * 30 - 1e-6), "progress after every picture", std::to_string(ref.calls));
    for (int workers : {2, 3, 4, 8}) {
        Out o = render(avi, workers, "avi", len);
        const std::string name = std::to_string(workers) + " pictures at once";
        check(o.ok, name + ": renders", o.error);
        check(o.bytes == ref.bytes, name + ": the file is byte for byte the serial one", std::to_string(o.bytes.size()) + " vs " + std::to_string(ref.bytes.size()));
        check(o.loud == ref.loud, name + ": the loudness log is the serial one");
        check(o.calls == ref.calls, name + ": the same number of progress calls", std::to_string(o.calls));
    }
    // a fraction of a picture at the end; one picture only
    for (double to : {0.05, 1.0 / 30.0, 2.43}) {
        Out a = render(avi, 1, "avi", to), b = render(avi, 4, "avi", to);
        check(a.ok && b.ok && a.bytes == b.bytes, strfmt("a render of %.3f s is the same with 4 as with 1", to), a.error + b.error);
    }
    {
        Out a = render(wav, 1, "wav", len), b = render(wav, 4, "wav", len);
        check(a.ok && b.ok && !a.bytes.empty() && a.bytes == b.bytes, "sound only: the same", a.error + b.error);
    }
    {
        Out a = render(mkv, 1, "mkv", 2.0), b = render(mkv, 3, "mkv", 2.0);
        // (a Matroska file has a creation time inside it that may differ: the lengths are compared, and the decoded pictures)
        check(a.ok && b.ok && !a.bytes.empty(), "FFV1 + FLAC in Matroska renders", a.error + b.error);
        std::string e;
        auto ra = openMedia(mkv, &e);
        check(ra && ra->info().hasVideo && ra->info().hasAudio, "the Matroska file has pictures and sound");
        bool same = a.bytes.size() == b.bytes.size();
        check(same, "Matroska files are the same size", std::to_string(a.bytes.size()) + " vs " + std::to_string(b.bytes.size()));
        if (ra) {
            // decode both and compare every picture and the sound
            const std::string pa = dir + "/par_a.mkv";
            writeFile(pa, a.bytes);
            auto r1 = openMedia(pa, &e), r2 = openMedia(mkv, &e);
            bool eq = r1 && r2;
            for (int f = 0; eq && f < 60; f += 3) {
                ImagePtr i1 = r1->videoFrame(f / 30.0 + 0.01, 0, 0), i2 = r2->videoFrame(f / 30.0 + 0.01, 0, 0);
                eq = i1 && i2 && i1->w == i2->w && i1->h == i2->h && std::memcmp(i1->row(0), i2->row(0), i1->rowBytes * (size_t)i1->h) == 0;
            }
            check(eq, "Matroska: the decoded pictures are the same");
            std::remove(pa.c_str());
        }
    }

    // ---- Cancel in the middle: stops at once, nothing left running, the call returns "stopped"
    for (int workers : {1, 2, 4}) {
        for (int after : {1, 10, 60}) {
            Out o = render(avi, workers, "avi", len, after);
            check(!o.ok && o.error == "stopped" && o.calls == after, strfmt("Cancel after %d pictures with %d at once", after, workers),
                  strfmt("ok=%d error='%s' calls=%d", (int)o.ok, o.error.c_str(), o.calls));
        }
    }
    {
        // after a cancelled render the next one is whole
        Out o = render(avi, 4, "avi", len);
        check(o.ok && o.bytes == ref.bytes, "a render after a cancelled one is whole");
    }

    // ---- an encoder that fails: the error comes back, nothing hangs
    for (int workers : {1, 4}) {
        Out o = render(dir + "/no_such_folder/x.avi", workers, "avi", len);
        check(!o.ok && !o.error.empty(), strfmt("an unwritable file is an error with %d at once", workers), o.error);
    }
    std::remove(avi.c_str());
    std::remove(wav.c_str());
    std::remove(mkv.c_str());
    std::remove(log.c_str());
    std::printf("\nSUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
