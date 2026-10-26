// Tests the kinds of media file Reno reads: every container, codec and picture format of the import list that this
// machine can make a file of, each made here and read back through openMedia().
//
//   formats_test <out dir> [--ffmpeg <program>] [--only <text>]
//
// A test clip is made with Reno's own writer: 60 numbered pictures (PNG with transparency, 320x240, 30 fps) and two
// seconds of sound (WAV, 48 kHz stereo). The ffmpeg program (the Linux one; --ffmpeg names another) turns them into
// each sample file; a sample whose encoder or writer this ffmpeg lacks is skipped (SKIP, not a failure). Each sample is
// opened and checked: what it reports (pictures and/or sound, a still or not, size, frame rate, length, channels,
// sample rate, field order, bit depth, transparency), the first, a middle and the last picture (by the frame number
// each picture carries as eight black or white squares) read in order and by jumping about, the colours of six patches,
// transparency where the format keeps it, and the sound: its pitch on each side, three clicks in the right place
// (after VEGAS's AAC delay, MediaInfo::audioDelaySamples), and a stretch read after a seek against the same stretch
// read straight through.
//
// The picture: grey; the frame number as eight squares (most significant bit on the left) along the top; red, green,
// blue, yellow, cyan and magenta patches below them; a transparent rectangle at the right; an orange square in the
// bottom left corner and a purple one in the bottom right. The sound: 440 Hz on the left, 880 Hz on the right (0.25),
// with a click (24 samples at +0.9, then 24 at -0.9) at 0.5, 1.0 and 1.5 seconds.
//
// One line per check ("PASS"/"FAIL"/"SKIP"/"INFO"), a summary, the same in <out dir>/formats_log.txt; exit code 0
// only if nothing failed.
#include "../../src/media/media.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>

using namespace reno;

namespace {

FILE* gLog = nullptr;
int gPassed = 0, gFailed = 0, gSkipped = 0;
std::string gOut, gFfmpeg = "ffmpeg";

void emit(const std::string& line)
{
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (gLog) {
        std::fprintf(gLog, "%s\n", line.c_str());
        std::fflush(gLog);
    }
}
void note(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const std::string s = vstrfmt(fmt, ap);
    va_end(ap);
    emit("INFO " + s);
}
void skip(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const std::string s = vstrfmt(fmt, ap);
    va_end(ap);
    emit("SKIP " + s);
    ++gSkipped;
}
bool check(bool ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const std::string s = vstrfmt(fmt, ap);
    va_end(ap);
    emit((ok ? "PASS " : "FAIL ") + s);
    if (ok) ++gPassed;
    else ++gFailed;
    return ok;
}

// ---------------------------------------------------------------- the test picture
const int kW = 320, kH = 240, kFrames = 60, kFps = 30, kRate = 48000, kSamples = kRate * 2;
struct Rect { int x, y, w, h; };            // top-down coordinates in the 320x240 picture
struct Colour { int b, g, r; };
Rect bitRect(int i) { return Rect{4 + i * 39, 8, 32, 32}; }
const int kPatches = 6;
Rect patchRect(int i) { return Rect{8 + i * 52, 56, 40, 40}; }
const Colour kPatch[kPatches] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {0, 255, 255}, {255, 255, 0}, {255, 0, 255}};
const char* const kPatchName[kPatches] = {"red", "green", "blue", "yellow", "cyan", "magenta"};
const Rect kClear = {200, 110, 100, 80};    // transparent
const Rect kBottomLeft = {0, 216, 24, 24}, kBottomRight = {296, 216, 24, 24};
const Colour kGrey = {96, 96, 96}, kOrange = {0, 128, 255}, kPurple = {200, 0, 160};

void fill(Image& img, const Rect& r, const Colour& c, int a = 255)
{
    for (int y = std::max(0, r.y); y < std::min(img.h, r.y + r.h); ++y) {
        uint8_t* row = img.row(img.h - 1 - y);
        for (int x = std::max(0, r.x); x < std::min(img.w, r.x + r.w); ++x) {
            row[4 * x + 0] = (uint8_t)(c.b * a / 255);      // (premultiplied)
            row[4 * x + 1] = (uint8_t)(c.g * a / 255);
            row[4 * x + 2] = (uint8_t)(c.r * a / 255);
            row[4 * x + 3] = (uint8_t)a;
        }
    }
}

ImagePtr makeFrame(int n)
{
    ImagePtr img = Image::make(kW, kH, PixelDepth::Byte, false);
    fill(*img, Rect{0, 0, kW, kH}, kGrey);
    for (int i = 0; i < 8; ++i) fill(*img, bitRect(i), (n >> (7 - i)) & 1 ? Colour{255, 255, 255} : Colour{0, 0, 0});
    for (int i = 0; i < kPatches; ++i) fill(*img, patchRect(i), kPatch[i]);
    fill(*img, kClear, Colour{0, 0, 0}, 0);
    fill(*img, kBottomLeft, kOrange);
    fill(*img, kBottomRight, kPurple);
    return img;
}

struct Mean { double b, g, r, a; };
// The average of the middle of a rectangle (inset pixels in from its edges) of a picture of any size: rx, ry, rw, rh
// are in a kW x kH (or, for turned pictures, the given base size) frame.
Mean meanOf(const Image& img, const Rect& r, int inset, int baseW = kW, int baseH = kH)
{
    const double sx = img.w / (double)baseW, sy = img.h / (double)baseH;
    int x0 = (int)std::ceil((r.x + inset) * sx), x1 = (int)std::floor((r.x + r.w - inset) * sx);
    int y0 = (int)std::ceil((r.y + inset) * sy), y1 = (int)std::floor((r.y + r.h - inset) * sy);
    x0 = std::max(0, std::min(img.w - 1, x0));
    y0 = std::max(0, std::min(img.h - 1, y0));
    x1 = std::max(x0 + 1, std::min(img.w, x1));
    y1 = std::max(y0 + 1, std::min(img.h, y1));
    Mean m = {0, 0, 0, 0};
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            double b, g, rr, a;
            if (img.depth == PixelDepth::Float) {
                const float* p = (const float*)img.row(img.h - 1 - y) + 4 * x;      // R G B A
                rr = p[0] * 255.0; g = p[1] * 255.0; b = p[2] * 255.0; a = p[3] * 255.0;
            } else {
                const uint8_t* p = img.row(img.h - 1 - y) + 4 * x;
                b = p[0]; g = p[1]; rr = p[2]; a = p[3];
            }
            m.b += b; m.g += g; m.r += rr; m.a += a;
        }
    }
    const double n = (double)(x1 - x0) * (y1 - y0);
    m.b /= n; m.g /= n; m.r /= n; m.a /= n;
    return m;
}
double distance(const Mean& m, const Colour& c) { return std::max(std::fabs(m.b - c.b), std::max(std::fabs(m.g - c.g), std::fabs(m.r - c.r))); }

int frameNumber(const Image* img)
{
    if (!img || img->w < 32 || img->h < 24) return -1;
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        const Mean m = meanOf(*img, bitRect(i), 9);
        const double v = (m.b + m.g + m.r) / 3.0;
        if (v > 150.0) n |= 1 << (7 - i);
        else if (v > 100.0) return -1;
    }
    return n;
}

// ---------------------------------------------------------------- the test sound
const double kClickAt[3] = {0.5, 1.0, 1.5};
void makeSound(std::vector<float>& l, std::vector<float>& r)
{
    l.assign(kSamples, 0.f);
    r.assign(kSamples, 0.f);
    for (int i = 0; i < kSamples; ++i) {
        const double t = i / (double)kRate;
        l[i] = (float)(0.25 * std::sin(2 * M_PI * 440 * t));
        r[i] = (float)(0.25 * std::sin(2 * M_PI * 880 * t));
    }
    for (double c : kClickAt)
        for (int k = 0; k < 48; ++k) l[(int)(c * kRate) + k] = r[(int)(c * kRate) + k] = k < 24 ? 0.9f : -0.9f;
}
int crossings(const std::vector<float>& x, int from, int to)
{
    int n = 0;
    for (int i = std::max(1, from); i < to && i < (int)x.size(); ++i)
        if (x[i - 1] < 0.f && x[i] >= 0.f) ++n;
    return n;
}
double rms(const std::vector<float>& x, int from, int to)
{
    double s = 0;
    int n = 0;
    for (int i = std::max(0, from); i < to && i < (int)x.size(); ++i, ++n) s += (double)x[i] * x[i];
    return n ? std::sqrt(s / n) : 0.0;
}
int biggestStep(const std::vector<float>& x, int from, int to)
{
    int best = -1;
    float size = 0;
    for (int i = std::max(1, from); i < to && i < (int)x.size(); ++i) {
        const float d = std::fabs(x[i] - x[i - 1]);
        if (d > size) { size = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------- making the samples
bool exists(const std::string& p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && st.st_size > 0;
}
std::string sh(const std::string& s) { return "'" + s + "'"; }

bool makeMaster()
{
    const std::string dir = pathJoin(gOut, "master");
    ::mkdir(dir.c_str(), 0755);
    EncodeSettings s;
    s.path = pathJoin(dir, "f_%03d.png");
    s.audio = false;
    s.width = kW;
    s.height = kH;
    s.fpsNum = kFps;
    s.videoFamily = "png";
    s.alpha = true;
    std::string e;
    std::unique_ptr<MediaWriter> w = createMediaWriter(s, &e);
    if (!check(w != nullptr, "the test pictures can be written (%s)", e.c_str())) return false;
    for (int n = 0; n < kFrames; ++n)
        if (!w->writeVideo(*makeFrame(n))) return check(false, "writing test picture %d", n);
    if (!check(w->finish(&e), "the test pictures are written %s", e.c_str())) return false;
    EncodeSettings a;
    a.path = pathJoin(dir, "sound.wav");
    a.video = false;
    a.sampleRate = kRate;
    a.audioBits = 24;
    w = createMediaWriter(a, &e);
    if (!check(w != nullptr, "the test sound can be written (%s)", e.c_str())) return false;
    std::vector<float> l, r;
    makeSound(l, r);
    w->writeAudio(l.data(), r.data(), kSamples);
    return check(w->finish(&e), "the test sound is written %s", e.c_str());
}

// One sample file: how ffmpeg makes it and what Reno must find in it.
struct Sample {
    std::string file;               // its name in <out dir>/samples
    std::string make;               // ffmpeg's arguments; {V} the pictures (an input, at 30 fps), {V25} at 25 fps,
                                    // {VNTSC} at 29.97, {S} the first picture, {A} the sound, {OUT} the file, {DIR}
    bool video = true, audio = true, still = false, alpha = false;
    int w = kW, h = kH;
    double fps = kFps;
    double seconds = 2.0;
    int frames = kFrames;           // pictures in the file (each carrying its number)
    int channels = 2, rate = kRate;
    bool mono = false;              // the sound is the test sound mixed to one channel
    double colourTol = 40;
    double clickTolMs = 2.5;
    bool useRef = true;             // sound alone: ffmpeg's decoding says where the clicks are
    bool refAV = false;             // ...and with pictures too (both streams start at 0)
    bool brokenTimes = false;       // every packet has the same time stamp (FFmpeg's own RealAudio writer): no seeking, no clicks
    double clickShiftMs = 0;        // pictures and sound: an encoder delay the file keeps no note of (sound alone: ffmpeg's decoding says where)
    double seekTol = 0.03;          // how far sound read after a seek may be from the same read straight through
    std::string field = "none";
    int bits = 8;
    std::string why;                // what it is about, for the log
    bool numbered = true;           // pictures carry readable numbers
    bool knownUnreadable = false;   // FFmpeg (6.1 and 7.0) cannot read it: reported, not failed
    std::string shell;              // a whole shell command instead of 'make' (for pipes)
};

std::string expand(std::string s, const std::string& out)
{
    const std::string m = pathJoin(gOut, "master");
    auto rep = [&](const std::string& key, const std::string& val) {
        for (size_t p; (p = s.find(key)) != std::string::npos;) s.replace(p, key.size(), val);
    };
    rep("{V25}", "-framerate 25 -i " + sh(pathJoin(m, "f_%03d.png")));
    rep("{VNTSC}", "-framerate 30000/1001 -i " + sh(pathJoin(m, "f_%03d.png")));
    rep("{V}", "-framerate 30 -i " + sh(pathJoin(m, "f_%03d.png")));
    rep("{S}", "-i " + sh(pathJoin(m, "f_000.png")));
    rep("{A}", "-i " + sh(pathJoin(m, "sound.wav")));
    rep("{OUT}", sh(out));
    rep("{DIR}", pathJoin(gOut, "samples"));
    return s;
}

bool makeSample(const Sample& s, const std::string& out)
{
    std::string cmd;
    if (!s.shell.empty()) cmd = expand(s.shell, out);
    else cmd = gFfmpeg + " -nostdin -hide_banner -v error -y " + expand(s.make, out);
    cmd += " > " + sh(out + ".log") + " 2>&1";
    if (!s.shell.empty()) cmd = "( " + expand(s.shell, out) + " ) 2> " + sh(out + ".log");
    std::remove(out.c_str());
    const int rc = std::system(cmd.c_str());
    return rc == 0 && exists(out);
}

// ---------------------------------------------------------------- reading them back
int gFresh = kW;
ImagePtr fresh(MediaReader& r, double t)
{
    if (gFresh > 120) gFresh -= 2;
    else gFresh = kW;
    return r.videoFrame(t, gFresh, kH * 4);
}

void checkPictures(MediaReader& r, const Sample& s)
{
    const char* n = s.file.c_str();
    const MediaInfo& in = r.info();
    // (pictures that are not 320x240 - DV - are scaled; numbers are read the same way)
    if (s.still) {
        ImagePtr img = r.videoFrame(0, 0, 0);
        if (!check(img != nullptr, "%s gives its picture", n)) return;
        if (s.numbered) check(frameNumber(img.get()) == 0, "%s shows the right picture (got number %d)", n, frameNumber(img.get()));
        ImagePtr later = r.videoFrame(5.0, 0, 0);
        check(later && frameNumber(later.get()) == frameNumber(img.get()), "%s shows the same picture at any time", n);
    } else if (s.numbered) {
        int wrong = 0;
        std::string firstWrong;
        for (int i = 0; i < s.frames; ++i) {
            ImagePtr img = r.videoFrame(i / s.fps, 0, 0);
            const int got = frameNumber(img.get());
            if (got != i) {
                if (wrong < 5) firstWrong += strfmt(" %d->%d", i, got);
                ++wrong;
            }
        }
        check(wrong == 0, "%s pictures 0..%d read in order are the right ones%s", n, s.frames - 1,
              wrong ? strfmt(": %d wrong (asked->got%s)", wrong, firstWrong.c_str()).c_str() : "");
        const int at[] = {s.frames / 2, s.frames - 1, 0, s.frames * 3 / 4, s.frames / 6, s.frames / 2 + 1, s.frames / 2 - 1, s.frames - 1};
        wrong = 0;
        firstWrong.clear();
        for (int i : at) {
            const int got = frameNumber(fresh(r, (i + 0.5) / s.fps).get());
            if (got != i) {
                firstWrong += strfmt(" %d->%d", i, got);
                ++wrong;
            }
        }
        check(wrong == 0, "%s jumping about (middle, last, first, back and forth) gives the right pictures%s", n,
              wrong ? (": asked->got" + firstWrong).c_str() : "");
        const int past = frameNumber(fresh(r, s.seconds + 5.0).get());
        check(past == s.frames - 1, "%s a time past the end gives the last picture (%d)", n, past);
    }
    // colours and transparency of the first picture, at its own size (not for the turned one: rotation() checks that)
    if (in.width != s.w || s.w == kH) return;
    ImagePtr img = r.videoFrame(0, 0, 0);
    if (!img) return;
    double worst = 0;
    std::string worstName;
    for (int i = 0; i < kPatches; ++i) {
        const double d = distance(meanOf(*img, patchRect(i), 8), kPatch[i]);
        if (d > worst) { worst = d; worstName = kPatchName[i]; }
    }
    check(worst <= s.colourTol, "%s colours are right (the furthest, %s, is %.0f off; %.0f allowed)", n, worstName.c_str(), worst, s.colourTol);
    const Mean bg = meanOf(*img, Rect{0, 150, 150, 50}, 4);
    check(bg.a > 250, "%s opaque parts are opaque (alpha %.0f)", n, bg.a);
    if (s.alpha) {
        const Mean c = meanOf(*img, kClear, 6);
        check(c.a < 6, "%s keeps its transparency (alpha %.0f where it is clear)", n, c.a);
        if (ImagePtr deep = r.videoFrameInDepth(0, 0, 0, PixelDepth::Float)) {
            const Mean d = meanOf(*deep, kClear, 6), o = meanOf(*deep, Rect{0, 150, 150, 50}, 4);
            check(d.a < 6 && o.a > 250, "%s keeps its transparency in 32-bit (float) projects too (%.0f / %.0f)", n, d.a, o.a);
        }
    } else if (in.hasVideo) {
        const Mean c = meanOf(*img, kClear, 6);
        check(c.a > 250, "%s has no transparency where the format keeps none (alpha %.0f)", n, c.a);
    }
}

void checkSound(MediaReader& r, const Sample& s)
{
    const char* n = s.file.c_str();
    const MediaInfo& in = r.info();
    std::vector<float> l(kSamples), rr(kSamples);
    if (!check(r.audio(0, kSamples, kRate, l.data(), rr.data()), "%s gives its sound", n)) return;
    const int a = kRate / 10, b = kRate * 4 / 10;
    if (!s.mono) {
        const int cl = crossings(l, a, b), cr = crossings(rr, a, b);
        check(std::abs(cl - 132) <= 3 && std::abs(cr - 264) <= 4, "%s pitch is right on each side (%d and %d cycles in 0.3 s; 132 and 264 wanted)", n, cl, cr);
    }
    const double lv = rms(l, a, b);
    check(lv > (s.mono ? 0.08 : 0.15) && lv < (s.channels > 2 ? 1.0 : 0.3), "%s level is right (%.3f)", n, lv);
    if (s.brokenTimes) {
        note("%s has no usable time stamps: where its clicks are and seeking in it are not checked", n);
        return;
    }
    // the clicks, where they were put (or, for sound alone, where ffmpeg's own decoding of the file has them) less
    // VEGAS's AAC delay
    const double delay = in.sampleRate > 0 ? (double)in.audioDelaySamples * kRate / in.sampleRate : 0.0;
    std::vector<float> ref;
    if ((!s.video || s.refAV) && s.useRef) {
        const std::string raw = pathJoin(gOut, "samples/" + s.file + ".ref.raw");
        const std::string cmd = gFfmpeg + " -nostdin -v error -y -i " + sh(pathJoin(gOut, "samples/" + s.file)) + " -map 0:a:0 -ac 1 -ar 48000 -f f32le " + sh(raw);
        if (std::system(cmd.c_str()) == 0)
            if (FILE* f = std::fopen(raw.c_str(), "rb")) {
                ref.resize(kSamples * 2);
                ref.resize(std::fread(ref.data(), sizeof(float), ref.size(), f));
                std::fclose(f);
            }
    }
    std::string where;
    double worst = 0;
    for (double c : kClickAt) {
        double want = c * kRate + 24 + s.clickShiftMs * kRate / 1000.0;
        if (!ref.empty()) want = biggestStep(ref, (int)want - kRate / 12, (int)want + kRate / 12);
        want -= delay;
        const int got = biggestStep(l, (int)want - kRate / 20, (int)want + kRate / 20);
        const double off = std::fabs(got - want) * 1000.0 / kRate;
        worst = std::max(worst, off);
        where += strfmt(" %.1f", (got - want) * 1000.0 / kRate);
    }
    check(worst <= s.clickTolMs, "%s clicks are in place (off by%s ms; %.1f allowed)", n, where.c_str(), s.clickTolMs);
    // a stretch read after a seek back is the same as read straight through
    const int from = kRate * 6 / 5, len = kRate / 10;
    std::vector<float> l2(len), r2(len);
    r.audio(kSamples - 100, 100, kRate, l2.data(), r2.data());          // (somewhere else first)
    r.audio(from, len, kRate, l2.data(), r2.data());
    double diff = 0;
    for (int i = 0; i < len; ++i) diff = std::max(diff, (double)std::fabs(l2[i] - l[from + i]));
    check(diff <= s.seekTol, "%s sound read after a seek matches (largest difference %.4f)", n, diff);
    // and from far back to near the end
    std::vector<float> l3(len), r3(len);
    r.audio(1000, 100, kRate, l3.data(), r3.data());
    const int late = kRate * 17 / 10;
    r.audio(late, len, kRate, l3.data(), r3.data());
    diff = 0;
    for (int i = 0; i < len; ++i) diff = std::max(diff, (double)std::fabs(l3[i] - l[late + i]));
    check(diff <= s.seekTol, "%s sound read after a seek forward matches (largest difference %.4f)", n, diff);
}

void runSample(const Sample& s)
{
    const std::string dir = pathJoin(gOut, "samples");
    const std::string path = pathJoin(dir, s.file);
    const char* n = s.file.c_str();
    if (!makeSample(s, path)) {
        skip("%s: this ffmpeg cannot make it (%s)", n, s.why.c_str());
        return;
    }
    note("%s: %s", n, s.why.c_str());
    std::string err;
    std::unique_ptr<MediaReader> r = openMedia(path, &err);
    if (s.knownUnreadable) {
        if (r) check(r->videoFrame(0, 0, 0) != nullptr, "%s opens and gives a picture", n);
        else note("%s cannot be read with this FFmpeg: %s", n, err.c_str());
        return;
    }
    if (!check(r != nullptr, "%s opens%s%s", n, r ? "" : ": ", err.c_str())) return;
    const MediaInfo& in = r->info();
    note("%s: %s%s %dx%d %.3f fps %.3f s, %s %d Hz %d ch, %s, %d bit", n, in.hasVideo ? in.videoCodec.c_str() : "-", in.isStill ? " (still)" : "",
         in.width, in.height, in.fps, in.duration, in.hasAudio ? in.audioCodec.c_str() : "-", in.sampleRate, in.channels, in.fieldOrder.c_str(), in.bitDepth);
    check(in.hasVideo == s.video && in.hasAudio == s.audio, "%s has %s", n,
          s.video && s.audio ? "pictures and sound" : s.video ? "pictures only" : "sound only");
    check(in.isStill == s.still, "%s is %s", n, s.still ? "a still picture" : "not a still picture");
    if (s.video) {
        check(in.width == s.w && in.height == s.h, "%s is %dx%d (says %dx%d)", n, s.w, s.h, in.width, in.height);
        if (!s.still && s.fps > 0) check(std::fabs(in.fps - s.fps) < 0.01, "%s is %.3f fps (says %.3f)", n, s.fps, in.fps);
        check(in.fieldOrder == s.field, "%s field order is %s (says %s)", n, s.field.c_str(), in.fieldOrder.c_str());
        if (s.bits > 8) check(in.bitDepth == s.bits, "%s is %d-bit (says %d)", n, s.bits, in.bitDepth);
        check(in.hasAlpha == s.alpha, "%s says it %s transparency", n, s.alpha ? "has" : "has no");
    }
    if (s.audio) check(in.channels == s.channels && in.sampleRate == s.rate, "%s sound is %d channels at %d Hz (says %d at %d)", n, s.channels, s.rate, in.channels, in.sampleRate);
    if (!s.still) check(std::fabs(in.duration - s.seconds) <= 0.1, "%s lasts %.2f s (says %.3f)", n, s.seconds, in.duration);
    if (s.video) checkPictures(*r, s);
    if (s.audio) checkSound(*r, s);
}

// ---------------------------------------------------------------- the samples
std::vector<Sample> samples()
{
    std::vector<Sample> v;
    auto av = [&](const std::string& file, const std::string& args, const std::string& why) -> Sample& {
        Sample s;
        s.file = file;
        s.make = "{V} {A} " + args + " {OUT}";
        s.why = why;
        v.push_back(s);
        return v.back();
    };
    auto vid = [&](const std::string& file, const std::string& args, const std::string& why) -> Sample& {
        Sample& s = av(file, args, why);
        s.make = "{V} " + args + " {OUT}";
        s.audio = false;
        return s;
    };
    auto aud = [&](const std::string& file, const std::string& args, const std::string& why) -> Sample& {
        Sample& s = av(file, args, why);
        s.make = "{A} " + args + " {OUT}";
        s.video = false;
        return s;
    };
    auto pic = [&](const std::string& file, const std::string& args, const std::string& why) -> Sample& {
        Sample& s = av(file, args, why);
        s.make = "{S} " + args + " {OUT}";
        s.audio = false;
        s.still = true;
        return s;
    };
    const std::string x264 = "-c:v libx264 -preset veryfast -pix_fmt yuv420p -g 15";
    // ---- video with sound
    av("h264_aac.mp4", x264 + " -c:a aac -b:a 160k", "MP4: H.264 and AAC");
    av("h264_aac.m4v", x264 + " -c:a aac -f ipod", "M4V (iTunes MP4)");
    av("h264_aac.mov", x264 + " -c:a aac", "QuickTime: H.264 and AAC");
    av("hevc_aac.mp4", "-c:v libx265 -x265-params log-level=error -preset fast -pix_fmt yuv420p -tag:v hvc1 -c:a aac", "MP4: HEVC and AAC");
    { Sample& s = av("prores4444_alpha.mov", "-c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le -c:a pcm_s24le", "QuickTime: ProRes 4444 with alpha, 24-bit PCM");
      s.alpha = true; s.bits = 12; s.colourTol = 12; s.clickTolMs = 0.1; s.seekTol = 1e-4; }
    { Sample& s = av("prores422.mov", "-c:v prores_ks -profile:v 2 -c:a pcm_s16le", "QuickTime: ProRes 422");
      s.bits = 10; s.colourTol = 15; s.clickTolMs = 0.1; s.seekTol = 1e-3; }
    { Sample& s = av("qtrle_alpha.mov", "-c:v qtrle -pix_fmt argb -c:a pcm_s16be", "QuickTime: Animation (QTRLE) with alpha");
      s.alpha = true; s.colourTol = 2; s.clickTolMs = 0.1; s.seekTol = 1e-3; }
    { Sample& s = av("png_alpha.mov", "-c:v png -pix_fmt rgba -c:a pcm_s16le", "QuickTime: PNG with alpha");
      s.alpha = true; s.colourTol = 2; s.clickTolMs = 0.1; s.seekTol = 1e-3; }
    av("h264_flac.mkv", x264 + " -c:a flac", "Matroska: H.264 and FLAC").clickTolMs = 0.1;
    av("vp9_opus.mkv", "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M -pix_fmt yuv420p -c:a libopus", "Matroska: VP9 and Opus").clickTolMs = 0.3;
    av("vp9_opus.webm", "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M -pix_fmt yuv420p -c:a libopus", "WebM: VP9 and Opus").clickTolMs = 0.3;
    av("vp8_vorbis.webm", "-c:v libvpx -deadline realtime -cpu-used 8 -b:v 1M -pix_fmt yuv420p -c:a libvorbis", "WebM: VP8 and Vorbis");
    { Sample& s = av("vp9_alpha.webm", "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M -pix_fmt yuva420p -auto-alt-ref 0 -c:a libopus", "WebM: VP9 with alpha (needs the libvpx decoder)");
      s.alpha = true; s.clickTolMs = 0.3; }
    { Sample& s = av("vp8_alpha.webm", "-c:v libvpx -deadline realtime -cpu-used 8 -b:v 1M -pix_fmt yuva420p -auto-alt-ref 0 -c:a libvorbis", "WebM: VP8 with alpha");
      s.alpha = true; }
    av("av1_opus.webm", "-c:v libsvtav1 -preset 12 -pix_fmt yuv420p -g 15 -c:a libopus", "WebM: AV1 and Opus").clickTolMs = 0.3;
    { Sample& s = av("streamed.webm", "", "WebM written to a pipe: no length, no index (a browser's MediaRecorder file)");
      s.shell = gFfmpeg + " -nostdin -v error {V} {A} -c:v libvpx -deadline realtime -cpu-used 8 -b:v 1M -pix_fmt yuv420p -c:a libopus -f webm - > {OUT}"; s.clickTolMs = 0.3; }
    { Sample& s = av("mpeg4_mp3.avi", "-c:v mpeg4 -q:v 3 -c:a libshine -b:a 192k", "AVI: MPEG-4 Part 2 and MP3 (the MP3 encoder's delay is in the file)"); s.refAV = true; }
    av("mjpeg_pcm.avi", "-c:v mjpeg -q:v 3 -c:a pcm_s16le", "AVI: Motion JPEG and PCM").clickTolMs = 0.1;
    av("wmv2_wma.wmv", "-c:v wmv2 -b:v 2M -c:a wmav2 -b:a 192k", "Windows Media: WMV8 and WMA2").clickTolMs = 3;
    av("wmv1_wma.asf", "-c:v wmv1 -b:v 2M -c:a wmav1 -b:a 192k", "ASF: WMV7 and WMA1").clickTolMs = 3;
    { Sample& s = av("flv1_mp3.flv", "-c:v flv1 -b:v 2M -c:a libmp3lame -ar 44100", "Flash Video: Sorenson H.263 and MP3"); s.rate = 44100; s.clickTolMs = 3; }
    av("h264_aac.f4v", x264 + " -c:a aac -f f4v", "F4V: H.264 and AAC");
    av("mpeg1_mp2.mpg", "-c:v mpeg1video -b:v 3M -c:a mp2 -b:a 224k", "MPEG-1 program stream").clickTolMs = 3;
    av("mpeg2_mp2.mpeg", "-c:v mpeg2video -b:v 4M -c:a mp2 -b:a 224k", "MPEG-2 program stream").clickTolMs = 3;
    { Sample& s = vid("mpeg2.m2v", "-c:v mpeg2video -b:v 4M", "MPEG-2 elementary stream"); (void)s; }
    av("mpeg2_ac3.vob", "-c:v mpeg2video -b:v 4M -c:a ac3 -f vob", "DVD VOB: MPEG-2 and AC-3");
    av("h264_aac.ts", x264 + " -c:a aac", "MPEG transport stream: H.264 and AAC");
    av("h264_ac3.mts", x264 + " -c:a ac3 -f mpegts", "AVCHD .mts: H.264 and AC-3");
    av("h264_ac3.m2ts", x264 + " -c:a ac3 -f mpegts -mpegts_m2ts_mode 1", "Blu-ray .m2ts: H.264 and AC-3");
    av("h264_aac.3gp", x264 + " -c:a aac", "3GP: H.264 and AAC");
    av("h264_aac.3g2", x264 + " -c:a aac", "3G2: H.264 and AAC");
    av("theora_vorbis.ogv", "-c:v libtheora -q:v 7 -c:a libvorbis", "Ogg: Theora and Vorbis");
    av("theora_opus.ogg", "-c:v libtheora -q:v 7 -c:a libopus", "Ogg (.ogg) with pictures: Theora and Opus");
    av("mpeg2_pcm.mxf", "-c:v mpeg2video -b:v 8M -pix_fmt yuv422p -c:a pcm_s16le -ar 48000", "MXF: MPEG-2 4:2:2 and PCM").clickTolMs = 0.1;
    { Sample& s = av("ntsc.dv", "", "DV (NTSC, 4:1:1, lower field first)");
      s.make = "{VNTSC} {A} -vf scale=720:480,setsar=10/11 -c:v dvvideo -pix_fmt yuv411p -top 0 -c:a pcm_s16le -ar 48000 -f dv {OUT}";
      s.w = 720; s.h = 540; s.fps = 30000.0 / 1001; s.seconds = 60 / s.fps; s.frames = 59; s.field = "lower"; s.colourTol = 45; s.clickTolMs = 1; }
    { Sample& s = av("ffv1_pcm.nut", "-c:v ffv1 -pix_fmt bgra -c:a pcm_s16le", "NUT: FFV1 (with alpha) and PCM");
      s.alpha = true; s.colourTol = 2; s.clickTolMs = 0.1; s.seekTol = 1e-3; }
    { Sample& s = vid("yuv420.y4m", "-pix_fmt yuv420p", "YUV4MPEG (uncompressed)"); s.colourTol = 12; }
    { Sample& s = av("rv20_ac3.rm", "-c:v rv20 -b:v 2M -c:a ac3", "RealMedia: RealVideo 2 and AC-3 (its 256-sample encoder delay is in the file)"); s.clickShiftMs = 256 / 48.0; }
    { Sample& s = av("rv20_ac3.rmvb", "-c:v rv20 -b:v 2M -c:a ac3 -f rm", "RealMedia (.rmvb)"); s.clickShiftMs = 256 / 48.0; }
    vid("vp9.ivf", "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M", "IVF: VP9");
    { Sample& s = vid("av1.obu", "-c:v libsvtav1 -preset 12 -g 15", "AV1 low-overhead bitstream (.obu: no frame rate in it, FFmpeg says 25)"); s.fps = 25; s.seconds = 2.4; }
    vid("raw.h264", x264, "H.264 elementary stream");
    vid("raw.hevc", "-c:v libx265 -x265-params log-level=error -preset fast -pix_fmt yuv420p", "HEVC elementary stream");
    { Sample& s = vid("animated_alpha.gif", "", "animated GIF with transparency (25 fps: GIF counts in hundredths)");
      s.make = "{V25} -filter_complex \"split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse\" -loop 0 {OUT}";
      s.fps = 25; s.seconds = 2.4; s.alpha = true; s.colourTol = 30; }
    { Sample& s = vid("animated.apng", "-c:v apng -pix_fmt rgba -plays 0 -f apng", "animated PNG with alpha"); s.alpha = true; s.colourTol = 2; }
    { Sample& s = vid("animated_lossless.webp", "-c:v libwebp_anim -lossless 1 -loop 0", "animated WebP"); s.alpha = true; s.colourTol = 4; }
    // ---- the hard cases
    { Sample& s = av("rotated90.mp4", "", "a phone video turned on its side (display matrix: 90 degrees)");
      s.shell = gFfmpeg + " -nostdin -v error -y {V} {A} " + x264 + " -c:a aac {DIR}/unrotated.mp4 && " + gFfmpeg +
                " -nostdin -v error -y -display_rotation 90 -i {DIR}/unrotated.mp4 -c copy {OUT}";
      s.w = kH; s.h = kW; s.numbered = false; }
    { Sample& s = vid("vfr.mkv", "", "variable frame rate: 30 fps for a second, then 15 fps");
      s.make = "{V} -vf \"setpts='if(lt(N,30),N/30,1+(N-30)/15)/TB'\" -fps_mode passthrough " + x264 + " {OUT}";
      s.numbered = false; s.seconds = 3.0; s.fps = 0; }
    { Sample& s = vid("interlaced_tff.ts", "-c:v mpeg2video -b:v 6M -flags +ildct+ilme -top 1 -field_order tt", "interlaced MPEG-2, top field first");
      s.field = "upper"; }
    { Sample& s = vid("hdr10_pq.mkv", "", "10-bit HEVC, HDR10 (PQ, BT.2020): shown converted to SDR");
      s.make = "{V} -vf \"zscale=tin=iec61966-2-1:min=gbr:pin=709:t=smpte2084:m=2020_ncl:p=2020:npl=203:rangein=full:range=limited,format=yuv420p10le\" "
               "-c:v libx265 -x265-params log-level=error -preset fast -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc {OUT}";
      s.bits = 10; s.colourTol = 40; }
    { Sample& s = vid("hlg.mp4", "", "10-bit HEVC, HLG (BT.2020): shown converted to SDR");
      s.make = "{V} -vf \"zscale=tin=iec61966-2-1:min=gbr:pin=709:t=arib-std-b67:m=2020_ncl:p=2020:npl=203:rangein=full:range=limited,format=yuv420p10le\" "
               "-c:v libx265 -x265-params log-level=error -preset fast -color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc -tag:v hvc1 {OUT}";
      s.bits = 10; s.colourTol = 40; }
    { Sample& s = vid("deep10.mkv", "-c:v ffv1 -pix_fmt yuv444p10le", "10-bit lossless (FFV1)"); s.bits = 10; s.colourTol = 6; }
    // ---- sound
    aud("pcm16.wav", "-c:a pcm_s16le", "WAV, 16-bit").clickTolMs = 0.1;
    aud("pcm32f.w64", "-c:a pcm_f32le", "Wave64, 32-bit float").clickTolMs = 0.1;
    aud("pcm16.aif", "-c:a pcm_s16be", "AIFF, 16-bit").clickTolMs = 0.1;
    aud("pcm24.aiff", "-c:a pcm_s24be", "AIFF, 24-bit").clickTolMs = 0.1;
    aud("float.aifc", "-c:a pcm_f32be -f aiff", "AIFF-C, 32-bit float").clickTolMs = 0.1;
    aud("lame.mp3", "-c:a libmp3lame -b:a 192k", "MP3").clickTolMs = 3;
    aud("layer2.mp2", "-c:a mp2 -b:a 256k", "MPEG Layer II").clickTolMs = 3;
    aud("aac.m4a", "-c:a aac -b:a 192k", "AAC in MP4 (.m4a)");
    aud("adts.aac", "-c:a aac -b:a 192k", "AAC (ADTS, no container)").clickTolMs = 3;
    aud("lossless.flac", "-c:a flac", "FLAC").clickTolMs = 0.1;
    aud("vorbis.ogg", "-c:a libvorbis -q:a 6", "Ogg Vorbis");
    aud("flac.oga", "-c:a flac -f ogg", "FLAC in Ogg (.oga)").clickTolMs = 0.1;
    aud("opus.opus", "-c:a libopus -b:a 160k", "Opus (Ogg; 312 samples of pre-skip)");
    aud("opus_native.mka", "-c:a opus -strict -2 -b:a 160k", "Matroska audio: FFmpeg's own Opus encoder (2.5 ms pre-skip)").clickTolMs = 0.2;
    { Sample& s = aud("vorbis.mka", "-c:a libvorbis", "Matroska audio: Vorbis (FFmpeg's writer puts the sound 2.67 ms late: its first packet at 0)"); s.useRef = false; s.clickShiftMs = 128 / 48.0; s.clickTolMs = 0.5; }
    { Sample& s = aud("wma2.wma", "-c:a wmav2 -b:a 192k", "Windows Media Audio (its encoder delay, 43 ms, is in the file)"); s.useRef = false; s.clickShiftMs = 43.33; s.clickTolMs = 1; }
    aud("dolby.ac3", "-c:a ac3 -b:a 256k", "AC-3");
    aud("dolby.eac3", "-c:a eac3 -b:a 256k", "E-AC-3");
    aud("dts.dts", "-c:a dca -strict -2 -b:a 768k", "DTS").clickTolMs = 3;
    aud("wavpack.wv", "-c:a wavpack", "WavPack").clickTolMs = 0.1;
    aud("tta.tta", "-c:a tta", "True Audio").clickTolMs = 0.1;
    aud("alac.caf", "-c:a alac", "Core Audio (ALAC)").clickTolMs = 0.1;
    aud("alac.m4a", "-c:a alac", "Apple Lossless in .m4a").clickTolMs = 0.1;
    aud("sun.au", "-c:a pcm_s16be", "Sun .au").clickTolMs = 0.1;
    aud("sun.snd", "-c:a pcm_s16be -f au", "Sun .snd").clickTolMs = 0.1;
    { Sample& s = aud("real.ra", "-c:a real_144 -ar 8000 -ac 1 -f rm", "RealAudio 1.0 (14.4)"); s.rate = 8000; s.channels = 1; s.mono = true; s.brokenTimes = true; }
    { Sample& s = aud("mono11k.wav", "-ac 1 -ar 11025 -c:a pcm_s16le", "WAV, mono at 11,025 Hz (an odd rate, resampled)"); s.rate = 11025; s.channels = 1; s.mono = true; s.clickTolMs = 0.3; }
    { Sample& s = aud("rate96k.flac", "-ar 96000 -c:a flac", "FLAC at 96 kHz"); s.rate = 96000; s.clickTolMs = 0.1; }
    { Sample& s = aud("rate22k.mp3", "-ar 22050 -c:a libmp3lame", "MP3 at 22,050 Hz"); s.rate = 22050; s.clickTolMs = 3; }
    { Sample& s = aud("surround71.wav", "", "WAV, 7.1 (eight channels, mixed down to stereo)");
      s.make = "{A} -filter_complex \"[0:a]channelsplit=channel_layout=stereo[l][r];[l]asplit=4[l1][l2][l3][l4];[r]asplit=4[r1][r2][r3][r4];"
               "[l1][r1][l2][r2][l3][r3][l4][r4]amerge=inputs=8,aformat=channel_layouts=7.1\" -c:a pcm_s16le {OUT}";
      s.channels = 8; s.clickTolMs = 0.1; }
    { Sample& s = aud("surround51.flac", "", "FLAC 5.1 (mixed down to stereo)");
      s.make = "{A} -filter_complex \"[0:a]pan=5.1|FL=c0|FR=c1|FC=0*c0|LFE=0*c0|BL=c0|BR=c1\" -c:a flac {OUT}";
      s.channels = 6; s.clickTolMs = 0.1; }
    { Sample& s = aud("cover.mp3", "", "MP3 with a cover picture (the picture is not shown as video)");
      s.make = "{A} {S} -map 0:a -map 1:v -c:a libmp3lame -c:v mjpeg -disposition:v attached_pic -id3v2_version 3 {OUT}";
      s.clickTolMs = 3; }
    { Sample& s = aud("cover.m4a", "", "AAC .m4a with cover art");
      s.make = "{A} {S} -map 0:a -map 1:v -c:a aac -c:v png -disposition:v attached_pic {OUT}"; }
    { Sample& s = aud("cover.flac", "", "FLAC with a cover picture");
      s.make = "{A} {S} -map 0:a -map 1:v -c:a flac -c:v png -disposition:v attached_pic {OUT}"; s.clickTolMs = 0.1; }
    // ---- pictures
    { Sample& s = pic("still_alpha.png", "-pix_fmt rgba", "PNG with alpha"); s.alpha = true; s.colourTol = 1; }
    pic("still.jpg", "-q:v 2", "JPEG").colourTol = 30;
    pic("still.jpeg", "-q:v 2", "JPEG (.jpeg)").colourTol = 30;
    pic("still.bmp", "-pix_fmt bgr24", "BMP").colourTol = 1;
    { Sample& s = pic("still_alpha.tif", "-pix_fmt rgba", "TIFF with alpha"); s.alpha = true; s.colourTol = 1; }
    pic("still.tiff", "-pix_fmt rgb24 -compression_algo lzw", "TIFF (LZW)").colourTol = 1;
    { Sample& s = pic("still.gif", "", "GIF, one picture"); s.colourTol = 30; s.make = "{S} -filter_complex \"split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse\" {OUT}"; s.alpha = true; }
    { Sample& s = pic("still_lossy.webp", "-c:v libwebp -quality 90", "WebP (lossy, alpha)"); s.colourTol = 30; s.alpha = true; }
    { Sample& s = pic("still_alpha.webp", "-c:v libwebp -lossless 1", "WebP (lossless, alpha)"); s.alpha = true; s.colourTol = 2; }
    { Sample& s = pic("still_alpha.tga", "-pix_fmt bgra", "Targa with alpha"); s.alpha = true; s.colourTol = 1; }
    { Sample& s = pic("linear_alpha.exr", "", "OpenEXR (linear, half float, alpha)");
      s.make = "{S} -vf \"zscale=tin=iec61966-2-1:t=linear,format=gbrapf32le\" -c:v exr -format 1 {OUT}"; s.alpha = true; s.colourTol = 6; }
    { Sample& s = pic("linear.hdr", "", "Radiance HDR (linear)");
      s.make = "{S} -vf \"zscale=tin=iec61966-2-1:t=linear,format=gbrpf32le\" -c:v hdr {OUT}"; s.colourTol = 6; }
    { Sample& s = pic("icon.ico", "-vf scale=256:192 -c:v png -pix_fmt rgba", "Windows icon (PNG inside)"); s.w = 256; s.h = 192; s.alpha = true; s.colourTol = 30; }
    pic("still.jp2", "-c:v jpeg2000 -pix_fmt rgb24", "JPEG 2000").colourTol = 30;
    pic("still.avif", "-c:v libaom-av1 -still-picture 1 -cpu-used 8 -pix_fmt yuv420p", "AVIF (needs an AV1 decoder)").colourTol = 40;
    pic("still.dpx", "-pix_fmt rgb48le", "DPX (16-bit)").colourTol = 10;
    pic("still.ppm", "", "PPM").colourTol = 1;
    pic("still.pcx", "", "PCX").colourTol = 1;
    { Sample& s = pic("still.sgi", "", "SGI (alpha)"); s.colourTol = 1; s.alpha = true; }
    { Sample& s = pic("still.qoi", "", "QOI (alpha)"); s.colourTol = 1; s.alpha = true; }
    return v;
}

// Image sequences (Open Still Image Sequence): the master PNGs and a JPEG copy of them, read as video.
void imageSequences()
{
    const std::string m = pathJoin(gOut, "master");
    for (int jpeg = 0; jpeg < 2; ++jpeg) {
        std::string first = pathJoin(m, "f_000.png");
        if (jpeg) {
            const std::string dir = pathJoin(gOut, "samples/jpgseq");
            ::mkdir(dir.c_str(), 0755);
            const std::string cmd = gFfmpeg + " -nostdin -v error -y -framerate 30 -i " + sh(pathJoin(m, "f_%03d.png")) + " -q:v 2 -start_number 0 " + sh(pathJoin(dir, "j_%03d.jpg"));
            if (std::system(cmd.c_str()) != 0) { skip("a JPEG image sequence: this ffmpeg cannot make it"); continue; }
            first = pathJoin(dir, "j_000.jpg");
        }
        const char* n = jpeg ? "JPEG sequence" : "PNG sequence";
        check(imageSequenceLength(first) == kFrames, "%s has %d pictures (%d)", n, kFrames, imageSequenceLength(first));
        std::string err;
        std::unique_ptr<MediaReader> r = openMedia(imageSequenceSpec(first, 24.0), &err);
        if (!check(r != nullptr, "%s opens as video at 24 fps %s", n, err.c_str())) continue;
        check(!r->info().isStill && std::fabs(r->info().fps - 24.0) < 0.01 && std::fabs(r->info().duration - kFrames / 24.0) < 0.05,
              "%s is %d frames at 24 fps (%.3f fps, %.3f s)", n, kFrames, r->info().fps, r->info().duration);
        int wrong = 0;
        for (int i : {0, 30, 59, 12, 45}) wrong += frameNumber(r->videoFrame((i + 0.5) / 24.0, 0, 0).get()) != i;
        check(wrong == 0, "%s gives the right pictures (first, middle, last, back and forth)", n);
        if (!jpeg) check(meanOf(*r->videoFrame(0, 0, 0), kClear, 6).a < 6, "%s keeps the pictures' transparency", n);
    }
}

// The rotated phone video against ffmpeg's own turning of it (its first picture, written turned).
void rotation()
{
    const std::string path = pathJoin(gOut, "samples/rotated90.mp4"), ref = pathJoin(gOut, "samples/rotated90_ffmpeg.png");
    if (!exists(path)) return;
    const std::string cmd = gFfmpeg + " -nostdin -v error -y -i " + sh(path) + " -frames:v 1 " + sh(ref);
    if (std::system(cmd.c_str()) != 0) { skip("ffmpeg cannot turn the rotated video itself"); return; }
    std::unique_ptr<MediaReader> mine = openMedia(path), theirs = openMedia(ref);
    if (!check(mine && theirs, "the rotated video and ffmpeg's turned picture open")) return;
    ImagePtr a = mine->videoFrame(0, 0, 0), b = theirs->videoFrame(0, 0, 0);
    if (!check(a && b && a->w == b->w && a->h == b->h, "the rotated video is shown at ffmpeg's size (%dx%d, ffmpeg %dx%d)", a ? a->w : 0, a ? a->h : 0, b ? b->w : 0, b ? b->h : 0)) return;
    // the corners and the middle of each edge, compared
    double worst = 0;
    for (int k = 0; k < 9; ++k) {
        const Rect r = {(k % 3) * (a->w - 20) / 2, (k / 3) * (a->h - 20) / 2, 20, 20};
        const Mean x = meanOf(*a, r, 3, a->w, a->h), y = meanOf(*b, r, 3, b->w, b->h);
        worst = std::max(worst, std::max(std::fabs(x.r - y.r), std::max(std::fabs(x.g - y.g), std::fabs(x.b - y.b))));
    }
    check(worst < 30, "the rotated video is turned the way ffmpeg turns it (largest difference %.0f)", worst);
}

// The variable frame rate file: 30 fps for the first second, then 15 fps.
void variableRate()
{
    const std::string path = pathJoin(gOut, "samples/vfr.mkv");
    if (!exists(path)) return;
    std::unique_ptr<MediaReader> r = openMedia(path);
    if (!r) return;
    int wrong = 0;
    std::string firstWrong;
    for (int n : {0, 15, 29, 30, 31, 45, 59, 40, 10}) {
        const double t = n < 30 ? n / 30.0 : 1.0 + (n - 30) / 15.0;
        const int got = frameNumber(r->videoFrame(t + 0.01, 0, 0).get());
        if (got != n) { ++wrong; firstWrong += strfmt(" %d->%d", n, got); }
    }
    check(wrong == 0, "vfr.mkv pictures are shown at their own times, not at a steady rate%s", wrong ? (": asked->got" + firstWrong).c_str() : "");
}

void extensions()
{
    // every sample's extension is in the import list (so the dialogs and the Explorer show it)
    std::string missing;
    for (const Sample& s : samples())
        if (!hasMediaExtension(s.file)) missing += " " + s.file;
    check(missing.empty(), "every sample's extension is offered by the import dialogs and the Explorer%s", missing.empty() ? "" : (": not" + missing).c_str());
    check(hasMediaExtension("a.MP4", MediaKind::Video) && hasMediaExtension("b.Flac", MediaKind::Audio) && hasMediaExtension("c.exr", MediaKind::Picture) &&
              !hasMediaExtension("d.txt") && !hasMediaExtension("noextension") && !hasMediaExtension("dir.mp4/file"),
          "media extensions are told apart by kind, in any case");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: formats_test <out dir> [--ffmpeg <program>] [--only <text>]\n");
        return 2;
    }
    gOut = argv[1];
    std::string only;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--ffmpeg")) gFfmpeg = argv[i + 1];
        else if (!std::strcmp(argv[i], "--only")) only = argv[i + 1];
    }
    ::mkdir(gOut.c_str(), 0755);
    ::mkdir(pathJoin(gOut, "samples").c_str(), 0755);
    gLog = std::fopen(pathJoin(gOut, "formats_log.txt").c_str(), "w");
    note("%s", mediaBackendVersion().c_str());
    if (!makeMaster()) return 1;
    extensions();
    const bool haveCli = std::system((gFfmpeg + " -version > /dev/null 2>&1").c_str()) == 0;
    if (!haveCli) {
        skip("no ffmpeg program (%s): only Reno's own files are tested", gFfmpeg.c_str());
    } else {
        for (const Sample& s : samples())
            if (only.empty() || s.file.find(only) != std::string::npos) runSample(s);
        if (only.empty()) {
            imageSequences();
            rotation();
            variableRate();
        }
    }
    emit(strfmt("SUMMARY %d passed, %d failed, %d skipped", gPassed, gFailed, gSkipped));
    if (gLog) std::fclose(gLog);
    return gFailed ? 1 : 0;
}
