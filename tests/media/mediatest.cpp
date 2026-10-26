// Self-test for the media reader and writer (src/media).
//
//   mediatest <out dir> [media file ...]
//
// Writes a short synthetic clip to <out dir> in each container, reads every one back and checks what comes out; then,
// for any media files named on the command line, prints what they are and times a few frames and a second of sound.
// One line per check, "PASS ..." or "FAIL ...", a summary at the end, and the same text in <out dir>/mediatest_log.txt.
// The exit code is 0 only if nothing failed.
//
// The clip is 3 seconds of 640x360 at 30 fps. Every frame carries its own number as a row of eight black or white
// squares (most significant bit on the left), a white bar that moves 6 pixels a frame, a row of flat colour patches,
// and corner markers: ORANGE at the BOTTOM left, blue at the top left, purple at the bottom right. The sound is 440 Hz
// on the left and 880 Hz on the right, with a click every half second.
#include "../../src/media/media.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <direct.h>
#  include <wchar.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

using namespace reno;

namespace {

// ---------------------------------------------------------------- reporting
FILE* gLog = nullptr;
int gPassed = 0, gFailed = 0;

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

double nowMs()
{
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- the test picture
const int kW = 640, kH = 360, kFps = 30, kFrames = 90, kRate = 48000;
const int kSamples = kRate * kFrames / kFps;        // 144000
const double kPi = 3.14159265358979323846;

struct Rect {
    int x, y, w, h;                 // in the 640x360 picture, y counted DOWN from the top
};
struct Colour {
    int b, g, r;
};

Rect bitRect(int i) { Rect r = {56 + i * 68, 24, 56, 56}; return r; }
Rect patchRect(int i) { Rect r = {56 + i * 68, 112, 56, 56}; return r; }
const int kPatches = 8;
const Colour kPatch[kPatches] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {0, 255, 255}, {255, 255, 0}, {255, 0, 255}, {255, 255, 255}, {0, 0, 0}};
const char* const kPatchName[kPatches] = {"red", "green", "blue", "yellow", "cyan", "magenta", "white", "black"};
const Rect kBand = {0, 200, kW, 48};
const int kBarW = 16;
int barX(int frame) { return (frame * 6) % (kW - kBarW); }
const Rect kBottomLeft = {0, kH - 40, 40, 40}, kTopLeft = {0, 0, 40, 40}, kBottomRight = {kW - 40, kH - 40, 40, 40};
const Colour kOrange = {0, 128, 255}, kBlue = {255, 64, 0}, kPurple = {200, 0, 160}, kGrey = {96, 96, 96};

// Fills a rectangle given in top-down coordinates. Image rows are stored bottom row first, so the
// picture's row y (from the top) is Image row h-1-y.
void fill(Image& img, const Rect& r, const Colour& c)
{
    for (int y = std::max(0, r.y); y < std::min(img.h, r.y + r.h); ++y) {
        uint8_t* row = img.row(img.h - 1 - y);
        for (int x = std::max(0, r.x); x < std::min(img.w, r.x + r.w); ++x) {
            row[4 * x + 0] = (uint8_t)c.b;
            row[4 * x + 1] = (uint8_t)c.g;
            row[4 * x + 2] = (uint8_t)c.r;
            row[4 * x + 3] = 255;
        }
    }
}

ImagePtr makeFrame(int n)
{
    ImagePtr img = Image::make(kW, kH, PixelDepth::Byte, false);
    if (!img) return nullptr;
    const Colour white = {255, 255, 255}, black = {0, 0, 0}, dark = {32, 32, 32};
    const Rect all = {0, 0, kW, kH};
    fill(*img, all, kGrey);
    fill(*img, kBand, dark);
    for (int i = 0; i < 8; ++i) fill(*img, bitRect(i), (n >> (7 - i)) & 1 ? white : black);
    for (int i = 0; i < kPatches; ++i) fill(*img, patchRect(i), kPatch[i]);
    const Rect bar = {barX(n), kBand.y, kBarW, kBand.h};
    fill(*img, bar, white);
    fill(*img, kBottomLeft, kOrange);
    fill(*img, kTopLeft, kBlue);
    fill(*img, kBottomRight, kPurple);
    return img;
}

struct Mean {
    double b, g, r, a;
};

// The average colour of the middle of a rectangle of the test picture ('inset' pixels in from its
// edges), in a decoded picture of any size.
Mean meanOf(const Image& img, const Rect& r, int inset)
{
    const double sx = img.w / (double)kW, sy = img.h / (double)kH;
    int x0 = (int)std::ceil((r.x + inset) * sx), x1 = (int)std::floor((r.x + r.w - inset) * sx);
    int y0 = (int)std::ceil((r.y + inset) * sy), y1 = (int)std::floor((r.y + r.h - inset) * sy);
    x0 = std::max(0, std::min(img.w - 1, x0));
    y0 = std::max(0, std::min(img.h - 1, y0));
    x1 = std::max(x0 + 1, std::min(img.w, x1));
    y1 = std::max(y0 + 1, std::min(img.h, y1));
    Mean m = {0, 0, 0, 0};
    for (int y = y0; y < y1; ++y) {
        const uint8_t* row = img.row(img.h - 1 - y);
        for (int x = x0; x < x1; ++x) {
            m.b += row[4 * x + 0];
            m.g += row[4 * x + 1];
            m.r += row[4 * x + 2];
            m.a += row[4 * x + 3];
        }
    }
    const double n = (double)(x1 - x0) * (y1 - y0);
    m.b /= n;
    m.g /= n;
    m.r /= n;
    m.a /= n;
    return m;
}

double distance(const Mean& m, const Colour& c)
{
    return std::max(std::fabs(m.b - c.b), std::max(std::fabs(m.g - c.g), std::fabs(m.r - c.r)));
}

// The frame number a decoded picture carries, or -1 if the squares cannot be read.
int frameNumber(const Image* img)
{
    if (!img || img->depth != PixelDepth::Byte || img->w < 64 || img->h < 36) return -1;
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        const Mean m = meanOf(*img, bitRect(i), 14);
        const double v = (m.b + m.g + m.r) / 3.0;
        if (v > 160.0) n |= 1 << (7 - i);
        else if (v > 96.0) return -1;
    }
    return n;
}

// Where the middle of the moving bar is (in 640-wide coordinates), or -1.
double barCentre(const Image& img)
{
    const double sx = img.w / (double)kW, sy = img.h / (double)kH;
    const int y = std::min(img.h - 1, (int)((kBand.y + kBand.h / 2) * sy));
    const uint8_t* row = img.row(img.h - 1 - y);
    double sum = 0, weight = 0;
    for (int x = 0; x < img.w; ++x) {
        const double v = (row[4 * x] + row[4 * x + 1] + row[4 * x + 2]) / 3.0;
        if (v > 128.0) {
            sum += (x + 0.5) * v;
            weight += v;
        }
    }
    return weight > 0 ? sum / weight / sx : -1.0;
}

// ---------------------------------------------------------------- the test sound
const int kClicks = 5;
int clickAt(int i) { return (i + 1) * kRate / 2; }          // 0.5, 1.0, 1.5, 2.0, 2.5 seconds
const int kClickLength = 48;                                // 24 samples up, 24 samples down: the big step is at +24

void makeSound(std::vector<float>& left, std::vector<float>& right)
{
    left.resize((size_t)kSamples);
    right.resize((size_t)kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const double t = i / (double)kRate;
        left[(size_t)i] = (float)(0.25 * std::sin(2.0 * kPi * 440.0 * t));
        right[(size_t)i] = (float)(0.25 * std::sin(2.0 * kPi * 880.0 * t));
    }
    for (int c = 0; c < kClicks; ++c)
        for (int k = 0; k < kClickLength; ++k) {
            const float v = k < kClickLength / 2 ? 0.9f : -0.9f;
            left[(size_t)(clickAt(c) + k)] = v;
            right[(size_t)(clickAt(c) + k)] = v;
        }
}

int upwardCrossings(const float* x, int from, int to)
{
    int n = 0;
    for (int i = std::max(1, from); i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++n;
    return n;
}

double rms(const float* x, int from, int to)
{
    double s = 0;
    for (int i = from; i < to; ++i) s += (double)x[i] * x[i];
    return to > from ? std::sqrt(s / (to - from)) : 0.0;
}

// The index of the biggest sample-to-sample step in x[from..to).
int biggestStep(const float* x, int from, int to)
{
    int best = -1;
    float size = 0.0f;
    for (int i = std::max(1, from); i < to; ++i) {
        const float d = std::fabs(x[i] - x[i - 1]);
        if (d > size) {
            size = d;
            best = i;
        }
    }
    return best;
}

float peak(const float* x, int from, int to)
{
    float p = 0.0f;
    for (int i = from; i < to; ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

// ---------------------------------------------------------------- writing
struct Tolerance {
    double clickMs;         // how far a click may be from where it was put
    int lag;                // how many samples a seek may be out compared with reading straight through
    double level;           // how much a sample read after a seek may differ from the same one read straight through
    double colour;          // how far a colour patch may be from what was written (0..255)
};

struct Clip {
    const char* file;
    bool video, audio;
    Tolerance tol;
    bool written;
};

std::string joinPath(const std::string& dir, const std::string& name)
{
    return pathJoin(dir, name);
}

bool writeClip(const std::string& path, bool video, bool audio, int first, int frames, const std::vector<float>& left, const std::vector<float>& right)
{
    const std::string name = pathFileName(path);
    EncodeSettings s;
    s.path = path;
    s.video = video;
    s.audio = audio;
    s.width = kW;
    s.height = kH;
    s.fpsNum = kFps;
    s.fpsDen = 1;
    s.sampleRate = kRate;
    s.videoKbps = 4000;
    s.audioKbps = 192;
    std::string error;
    const double t0 = nowMs();
    std::unique_ptr<MediaWriter> w = createMediaWriter(s, &error);
    if (!w) return check(false, "write %s: %s", name.c_str(), error.c_str());
    note("%s is written as %s", name.c_str(), w->describe().c_str());
    const int perFrame = kRate / kFps;
    bool ok = true;
    for (int n = 0; n < frames && ok; ++n) {
        if (video) {
            ImagePtr img = makeFrame(first + n);
            ok = img && w->writeVideo(*img);
        }
        if (ok && audio) ok = w->writeAudio(left.data() + (size_t)n * perFrame, right.data() + (size_t)n * perFrame, perFrame);
    }
    error.clear();
    const bool finished = w->finish(&error);
    w.reset();
    const bool good = ok && finished;
    return check(good, "write %s%s%s (%.0f ms)", name.c_str(), good ? "" : ": ", good ? "" : (error.empty() ? "a frame was refused" : error.c_str()), nowMs() - t0);
}

// ---------------------------------------------------------------- reading back: pictures
// The reader caches the pictures it has converted, by size, so a repeated request at the same size says nothing about
// seeking. This asks at a size nobody has asked for before (a little narrower each time), which forces a new decode.
int gFreshWidth = kW;
void newReader() { gFreshWidth = kW; }
ImagePtr freshFrame(MediaReader& r, double seconds)
{
    if (gFreshWidth > 200) gFreshWidth -= 2;
    return r.videoFrame(seconds, gFreshWidth, kH);
}

void expectFrame(MediaReader& r, const std::string& name, double seconds, int want, const char* what)
{
    ImagePtr img = freshFrame(r, seconds);
    const int got = frameNumber(img.get());
    check(img && got == want, "%s %s (%.4f s) gives frame %d%s", name.c_str(), what, seconds, want,
          !img ? ": no picture came back" : got == want ? "" : strfmt(": got frame %d", got).c_str());
}

void verifyPictures(MediaReader& r, const std::string& name, const Tolerance& tol)
{
    const char* n = name.c_str();

    // Straight through, asking for each frame by its own start time.
    {
        int wrong = 0, missing = 0;
        std::string firstWrong;
        const double t0 = nowMs();
        for (int i = 0; i < kFrames; ++i) {
            ImagePtr img = r.videoFrame(i / (double)kFps, kW, kH);
            const int got = frameNumber(img.get());
            if (!img) ++missing;
            if (got != i) {
                if (wrong < 6) firstWrong += strfmt(" %d->%d", i, got);
                ++wrong;
            }
        }
        const double ms = nowMs() - t0;
        check(wrong == 0, "%s frames 0..%d read in order are the right frames (%.1f ms a frame)%s", n, kFrames - 1, ms / kFrames,
              wrong ? strfmt(": %d wrong, %d missing; asked->got%s", wrong, missing, firstWrong.c_str()).c_str() : "");
    }
    // The middle of a frame is still that frame.
    {
        const int at[] = {0, 17, 44, 89};
        int wrong = 0;
        std::string firstWrong;
        for (int i : at) {
            const int got = frameNumber(freshFrame(r, (i + 0.5) / kFps).get());
            if (got != i) {
                firstWrong += strfmt(" %d->%d", i, got);
                ++wrong;
            }
        }
        check(wrong == 0, "%s a time in the middle of a frame gives that frame%s", n, wrong ? (": asked->got" + firstWrong).c_str() : "");
    }
    // Jumping about.
    expectFrame(r, name, 2.0, 60, "jumping forward");
    expectFrame(r, name, 0.5, 15, "jumping back");
    expectFrame(r, name, 0.0, 0, "the first frame");
    expectFrame(r, name, (kFrames - 1) / (double)kFps, kFrames - 1, "the last frame");
    expectFrame(r, name, 10.0, kFrames - 1, "a time past the end");
    expectFrame(r, name, -1.0, 0, "a time before the start");
    expectFrame(r, name, 2.9, 87, "a frame boundary that does not divide evenly");
    expectFrame(r, name, 1.0 + 1.0 / kFps, 31, "one frame on from a second");
    expectFrame(r, name, 1.0 - 1.0 / kFps, 29, "one frame back from a second");
    {
        // 40 jumps to frames picked by a fixed pseudo-random sequence.
        unsigned seed = 12345u;
        int wrong = 0;
        std::string firstWrong;
        const double t0 = nowMs();
        for (int i = 0; i < 40; ++i) {
            seed = seed * 1103515245u + 12345u;
            const int want = (int)((seed >> 16) % (unsigned)kFrames);
            const int got = frameNumber(freshFrame(r, want / (double)kFps).get());
            if (got != want) {
                if (wrong < 6) firstWrong += strfmt(" %d->%d", want, got);
                ++wrong;
            }
        }
        check(wrong == 0, "%s 40 random jumps all give the right frame (%.1f ms a jump)%s", n, (nowMs() - t0) / 40.0,
              wrong ? strfmt(": %d wrong; asked->got%s", wrong, firstWrong.c_str()).c_str() : "");
    }

    // Backwards, one frame at a time (as playing backwards asks for them), at a size not asked for before so that
    // nothing comes from the converted-picture cache: the reader keeps what it decodes after a seek back and takes
    // the earlier frames from that, rather than seeking and decoding the stretch from the key frame every time.
    {
        int wrong = 0;
        std::string firstWrong;
        const double t0 = nowMs();
        for (int i = kFrames - 1; i >= 0; --i) {
            const int got = frameNumber(r.videoFrame(i / (double)kFps, 301, kH).get());
            if (got != i) {
                if (wrong < 6) firstWrong += strfmt(" %d->%d", i, got);
                ++wrong;
            }
        }
        const double back = (nowMs() - t0) / kFrames;
        check(wrong == 0, "%s frames %d..0 read backwards are the right frames (%.1f ms a frame)%s", n, kFrames - 1, back,
              wrong ? strfmt(": %d wrong; asked->got%s", wrong, firstWrong.c_str()).c_str() : "");
        // and again forwards at yet another size, for comparison: going backwards should cost about the same
        const double t1 = nowMs();
        for (int i = 0; i < kFrames; ++i) r.videoFrame(i / (double)kFps, 303, kH);
        const double fwd = (nowMs() - t1) / kFrames;
        check(back < fwd * 4.0 + 2.0, "%s going backwards costs about what going forwards does (%.1f against %.1f ms a frame)", n, back, fwd);
    }

    // One frame looked at closely.
    ImagePtr img = r.videoFrame(45 / (double)kFps, kW, kH);
    if (!check(img && img->w == kW && img->h == kH && img->depth == PixelDepth::Byte, "%s a frame asked for at %dx%d comes back that size%s", n, kW, kH,
               img ? strfmt(" (got %dx%d)", img->w, img->h).c_str() : ": no picture"))
        return;
    {
        const Mean bl = meanOf(*img, kBottomLeft, 10), tl = meanOf(*img, kTopLeft, 10), br = meanOf(*img, kBottomRight, 10);
        const bool right = distance(bl, kOrange) < 40 && distance(tl, kBlue) < 40 && distance(br, kPurple) < 40;
        const char* why = "";
        if (!right) {
            if (distance(tl, kOrange) < 40 && distance(bl, kBlue) < 40) why = ": the picture is UPSIDE DOWN (the orange marker is at the top left)";
            else if (distance(br, kOrange) < 40) why = ": the picture is MIRRORED left to right";
            else why = ": the corner markers are not the colours that were written";
        }
        check(right, "%s the picture is the right way up (orange marker at the bottom left, blue at the top left)%s", n, why);
    }
    {
        double worst = 0;
        int worstPatch = 0;
        Mean worstMean = {0, 0, 0, 0};
        for (int i = 0; i < kPatches; ++i) {
            const Mean m = meanOf(*img, patchRect(i), 14);
            const double d = distance(m, kPatch[i]);
            if (d > worst) {
                worst = d;
                worstPatch = i;
                worstMean = m;
            }
        }
        check(worst <= tol.colour, "%s colours come back as written (worst is %s, off by %.1f of 255; allowed %.0f)%s", n, kPatchName[worstPatch], worst, tol.colour,
              worst <= tol.colour ? "" : strfmt(": wrote B%d G%d R%d, read B%.0f G%.0f R%.0f", kPatch[worstPatch].b, kPatch[worstPatch].g, kPatch[worstPatch].r,
                                                worstMean.b, worstMean.g, worstMean.r).c_str());
    }
    {
        const Rect all = {0, 0, kW, kH};
        const Mean m = meanOf(*img, all, 0);
        check(m.a > 254.99, "%s a picture with no transparency has alpha 255 everywhere (mean %.2f)", n, m.a);
    }
    {
        double worst = 0;
        const int at[] = {10, 50, 80};
        for (int f : at) {
            ImagePtr p = r.videoFrame(f / (double)kFps, kW, kH);
            const double c = p ? barCentre(*p) : -1000.0;
            worst = std::max(worst, std::fabs(c - (barX(f) + kBarW / 2.0)));
        }
        check(worst <= 3.0, "%s the moving bar is where it should be (worst %.1f pixels out)", n, worst);
    }
    // Other sizes.
    {
        ImagePtr small = r.videoFrame(1.0, 320, 180);
        check(small && small->w == 320 && small->h == 180 && frameNumber(small.get()) == 30, "%s scaled down to 320x180%s", n,
              small ? strfmt(" (got %dx%d, frame %d)", small->w, small->h, frameNumber(small.get())).c_str() : ": no picture");
        ImagePtr boxed = r.videoFrame(1.0, 1000, 1000);
        check(boxed && boxed->w == 1000 && (boxed->h == 562 || boxed->h == 563) && frameNumber(boxed.get()) == 30, "%s fitted into 1000x1000 keeps its shape%s", n,
              boxed ? strfmt(" (got %dx%d, frame %d)", boxed->w, boxed->h, frameNumber(boxed.get())).c_str() : ": no picture");
        ImagePtr tall = r.videoFrame(1.0, 2000, 180);
        check(tall && tall->w == 320 && tall->h == 180, "%s fitted into 2000x180 is limited by the height%s", n,
              tall ? strfmt(" (got %dx%d)", tall->w, tall->h).c_str() : ": no picture");
        if (small) {
            const Mean bl = meanOf(*small, kBottomLeft, 10);
            check(distance(bl, kOrange) < 40, "%s the scaled picture is the right way up too", n);
        }
    }
    {
        ImagePtr a = r.videoFrame(1.5, kW, kH), b = r.videoFrame(1.5, kW, kH);
        check(a && a.get() == b.get(), "%s asking for the same frame twice gives the same picture from the cache", n);
    }
}

// ---------------------------------------------------------------- reading back: sound
// Reads 'count' samples from 'start' in blocks of 'block'.
bool readSound(MediaReader& r, int64_t start, int count, int rate, int block, std::vector<float>& left, std::vector<float>& right)
{
    left.assign((size_t)count, 0.0f);
    right.assign((size_t)count, 0.0f);
    for (int done = 0; done < count;) {
        const int n = std::min(block, count - done);
        if (!r.audio(start + done, n, rate, left.data() + done, right.data() + done)) return false;
        done += n;
    }
    return true;
}

bool nearClick(int64_t sample)
{
    for (int c = 0; c < kClicks; ++c)
        if (sample > clickAt(c) - 1200 && sample < clickAt(c) + 1200) return true;
    return false;
}

// Compares a block read after a seek with the same stretch read straight through: finds the shift (in samples, within
// +-200) that makes them match best, and how far apart they still are at that shift. With 'skipClicks' the difference
// is not measured within 25 ms of a click: an AAC decoder fills the noisy bands of such a burst with random noise
// (perceptual noise substitution) that differs each time decoding starts somewhere else.
void compareBlocks(const std::vector<float>& seqL, const std::vector<float>& seqR, int64_t start, const std::vector<float>& l, const std::vector<float>& r,
                   bool skipClicks, int& bestLag, double& worst)
{
    const int n = (int)l.size(), range = 200, margin = 256;
    double bestErr = -1;
    bestLag = 0;
    for (int lag = -range; lag <= range; ++lag) {
        double err = 0;
        for (int i = margin; i < n - margin; ++i) {
            const int64_t j = start + i - lag;          // block[i] should be seq[start + i]; with a lag it is seq[start + i - lag]
            if (j < 0 || j >= (int64_t)seqL.size()) continue;
            const double dl = l[(size_t)i] - seqL[(size_t)j], dr = r[(size_t)i] - seqR[(size_t)j];
            err += dl * dl + dr * dr;
        }
        if (bestErr < 0 || err < bestErr) {
            bestErr = err;
            bestLag = lag;
        }
    }
    worst = 0;
    for (int i = margin; i < n - margin; ++i) {
        const int64_t j = start + i - bestLag;
        if (j < 0 || j >= (int64_t)seqL.size()) continue;
        if (skipClicks && nearClick(j)) continue;
        worst = std::max(worst, (double)std::fabs(l[(size_t)i] - seqL[(size_t)j]));
        worst = std::max(worst, (double)std::fabs(r[(size_t)i] - seqR[(size_t)j]));
    }
}

void verifySound(MediaReader& r, const std::string& name, const Tolerance& tol, bool exact, const std::vector<float>& origL, const std::vector<float>& origR)
{
    const char* n = name.c_str();
    // Straight through from the start, in blocks that are not the codec's frame size, a little past the end.
    const int total = kSamples + kRate / 10;
    std::vector<float> seqL, seqR;
    const double t0 = nowMs();
    if (!check(readSound(r, 0, total, kRate, 1000, seqL, seqR), "%s sound reads straight through (%.0f ms for %.1f s)", n, nowMs() - t0, total / (double)kRate)) return;

    const int a = (int)(0.6 * kRate), b = (int)(0.9 * kRate);     // a stretch with no click in it
    {
        const int zl = upwardCrossings(seqL.data(), a, b), zr = upwardCrossings(seqR.data(), a, b);
        check(std::abs(zl - 132) <= 2 && std::abs(zr - 264) <= 3, "%s 440 Hz is on the left and 880 Hz on the right (measured %.0f and %.0f Hz)", n, zl / 0.3, zr / 0.3);
        const double rl = rms(seqL.data(), a, b), rr = rms(seqR.data(), a, b), want = 0.25 / std::sqrt(2.0);
        check(std::fabs(rl - want) < 0.15 * want && std::fabs(rr - want) < 0.15 * want, "%s the level is as written (rms %.4f left, %.4f right, wrote %.4f)", n, rl, rr, want);
    }
    {
        std::string list;
        double worst = 0;
        for (int c = 0; c < kClicks; ++c) {
            const int at = biggestStep(seqL.data(), clickAt(c) - kRate / 20, clickAt(c) + kRate / 20);
            const double ms = (at - (clickAt(c) + kClickLength / 2)) * 1000.0 / kRate;
            list += strfmt(" %+.2f", ms);
            worst = std::max(worst, std::fabs(ms));
        }
        check(worst <= tol.clickMs, "%s the clicks are where they were put (out by%s ms; allowed %.1f)", n, list.c_str(), tol.clickMs);
    }
    check(peak(seqL.data(), kSamples + kRate / 20, total) < 0.02f && peak(seqR.data(), kSamples + kRate / 20, total) < 0.02f, "%s there is silence after the end", n);
    if (exact) {
        double worst = 0;
        for (int i = 0; i < kSamples; ++i) {
            worst = std::max(worst, (double)std::fabs(seqL[(size_t)i] - origL[(size_t)i]));
            worst = std::max(worst, (double)std::fabs(seqR[(size_t)i] - origR[(size_t)i]));
        }
        check(worst < 1e-4, "%s every sample is the one that was written (worst difference %.6f)", n, worst);
    }

    // Seeks: each block has a click in it, so there is no doubt about where it belongs.
    const int64_t starts[] = {95000, 47900, 23993, 119000, 71500, 95500};     // the last is a short hop forward, which is decoded through rather than seeked
    for (int64_t start : starts) {
        std::vector<float> l, rr;
        const double t1 = nowMs();
        if (!check(readSound(r, start, 4800, kRate, 4800, l, rr), "%s sound reads after a seek to sample %lld", n, (long long)start)) continue;
        const double ms = nowMs() - t1;
        int lag = 0;
        double worst = 0;
        compareBlocks(seqL, seqR, start, l, rr, !exact, lag, worst);
        check(std::abs(lag) <= tol.lag && worst <= tol.level, "%s a seek to sample %lld gives the same sound as reading straight through (out by %d samples, worst %.5f; allowed %d and %.4f; %.1f ms)", n,
              (long long)start, lag, worst, tol.lag, tol.level, ms);
    }
    // Before the start there is silence, and then the start.
    {
        std::vector<float> l, rr;
        if (check(readSound(r, -2400, 4800, kRate, 4800, l, rr), "%s sound reads from before the start", n)) {
            const float before = std::max(peak(l.data(), 0, 2400), peak(rr.data(), 0, 2400));
            double worst = 0;
            for (int i = 1200; i < 2400; ++i) worst = std::max(worst, (double)std::fabs(l[(size_t)(2400 + i)] - seqL[(size_t)i]));
            check(before == 0.0f && worst <= std::max(tol.level, 0.05), "%s before the start is silence and the sound then starts on time (worst difference %.5f)", n, worst);
        }
    }
    // Past the end there is silence.
    {
        std::vector<float> l(1000, 1.0f), rr(1000, 1.0f);
        const bool ok = r.audio((int64_t)kRate * 10, 1000, kRate, l.data(), rr.data());
        check(ok && peak(l.data(), 0, 1000) == 0.0f && peak(rr.data(), 0, 1000) == 0.0f, "%s far past the end is silence", n);
    }
    // Another sample rate.
    {
        const int rate = 44100;
        std::vector<float> l, rr;
        if (check(readSound(r, 0, rate * 12 / 10, rate, 4096, l, rr), "%s sound reads at 44100 Hz", n)) {
            const int zl = upwardCrossings(l.data(), (int)(0.6 * rate), (int)(0.9 * rate));
            const int at = biggestStep(l.data(), rate - rate / 20, rate + rate / 20);
            const double ms = (at - (rate + kClickLength / 2 * rate / (double)kRate)) * 1000.0 / rate;
            check(std::abs(zl - 132) <= 2 && std::fabs(ms) <= tol.clickMs + 0.1, "%s at 44100 Hz the pitch and timing are unchanged (%.0f Hz, click at 1 s out by %+.2f ms)", n, zl / 0.3, ms);
        }
        // and back again
        std::vector<float> l2, r2;
        if (readSound(r, 95000, 4800, kRate, 4800, l2, r2)) {
            int lag = 0;
            double worst = 0;
            compareBlocks(seqL, seqR, 95000, l2, r2, !exact, lag, worst);
            check(std::abs(lag) <= tol.lag && worst <= tol.level, "%s going back to 48000 Hz gives the same sound as before (out by %d samples, worst difference %.5f)", n, lag, worst);
        } else {
            check(false, "%s sound reads after going back to 48000 Hz", n);
        }
    }
}

// AAC in an MP4/MOV is read as VEGAS reads it: 2112 samples (MediaInfo::audioDelaySamples) into what is decoded, so
// what was written is that much earlier. Reading back through this undoes it so the checks compare like with like;
// the first 2112 samples written are what VEGAS never plays (silence here).
class UndelayedReader : public MediaReader {
public:
    UndelayedReader(MediaReader& r) : r_(r) {}
    const MediaInfo& info() const override { return r_.info(); }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override { return r_.videoFrame(seconds, maxW, maxH); }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        const int sr = r_.info().sampleRate;
        const int64_t d = sr > 0 ? ((int64_t)r_.info().audioDelaySamples * sampleRate + sr / 2) / sr : 0;
        return r_.audio(start - d, frames, sampleRate, left, right);
    }

private:
    MediaReader& r_;
};

// ---------------------------------------------------------------- reading back: a whole clip
void verifyClip(const std::string& path, const Clip& clip, const std::vector<float>& left, const std::vector<float>& right)
{
    const std::string name = pathFileName(path);
    const char* n = name.c_str();
    std::string error;
    std::unique_ptr<MediaReader> r = openMedia(path, &error);
    if (!check(r != nullptr, "open %s%s%s", n, r ? "" : ": ", r ? "" : error.c_str())) return;
    newReader();
    const MediaInfo& i = r->info();
    note("%s is %dx%d, %.3f fps, %.3f s, video %s, audio %s %d Hz %d channels%s", n, i.width, i.height, i.fps, i.duration,
         i.hasVideo ? i.videoCodec.c_str() : "none", i.hasAudio ? i.audioCodec.c_str() : "none", i.sampleRate, i.channels, i.isStill ? ", a still" : "");

    check(i.hasVideo == clip.video && i.hasAudio == clip.audio, "%s has %s", n, clip.video && clip.audio ? "pictures and sound" : clip.video ? "pictures only" : "sound only");
    check(!i.isStill && std::fabs(i.duration - kFrames / (double)kFps) <= 0.15, "%s lasts %.1f s (says %.3f)", n, kFrames / (double)kFps, i.duration);
    if (clip.video) {
        check(i.width == kW && i.height == kH, "%s is %dx%d (says %dx%d)", n, kW, kH, i.width, i.height);
        check(std::fabs(i.fps - kFps) < 0.01, "%s is %d fps (says %.4f)", n, kFps, i.fps);
    }
    if (clip.audio) check(i.sampleRate == kRate && i.channels == 2, "%s sound is %d Hz stereo (says %d Hz, %d channels)", n, kRate, i.sampleRate, i.channels);

    if (clip.video && i.hasVideo) {
        verifyPictures(*r, name, clip.tol);
    } else if (!clip.video) {
        check(r->videoFrame(1.0, kW, kH) == nullptr, "%s gives no picture, as it has none", n);
    }
    if (clip.audio && i.hasAudio) {
        const bool aac = i.audioCodec == "aac" && (endsWithNoCase(name, ".mp4") || endsWithNoCase(name, ".mov") || endsWithNoCase(name, ".m4a"));
        check(i.audioDelaySamples == (aac ? 2112 : 0), "%s sound starts %d samples into what is decoded (VEGAS's AAC delay; says %d)", n, aac ? 2112 : 0,
              i.audioDelaySamples);
        UndelayedReader u(*r);
        verifySound(u, name, clip.tol, clip.tol.lag == 0, left, right);
        if (clip.video && i.hasVideo) {
            // Pictures and sound from the same reader, turn and turn about, must not disturb each other.
            bool ok = true;
            std::vector<float> l(1600), rr(1600);
            for (int f = 30; f < 40 && ok; ++f) {
                ok = frameNumber(r->videoFrame(f / (double)kFps, 400, 225).get()) == f && u.audio((int64_t)f * 1600, 1600, kRate, l.data(), rr.data());
                if (ok && f == 30) ok = std::abs(biggestStep(l.data(), 0, 1600) - kClickLength / 2) <= (int)(clip.tol.clickMs * kRate / 1000.0) + 1;
            }
            check(ok, "%s pictures and sound can be read alternately from one reader", n);
        }
    } else if (!clip.audio) {
        std::vector<float> l(100, 1.0f), rr(100, 1.0f);
        check(!r->audio(0, 100, kRate, l.data(), rr.data()) && peak(l.data(), 0, 100) == 0.0f, "%s gives no sound, as it has none", n);
    }
}

void verifyStill(const std::string& path, int number)
{
    const std::string name = pathFileName(path);
    const char* n = name.c_str();
    std::string error;
    std::unique_ptr<MediaReader> r = openMedia(path, &error);
    if (!check(r != nullptr, "open %s%s%s", n, r ? "" : ": ", r ? "" : error.c_str())) return;
    const MediaInfo& i = r->info();
    note("%s is %dx%d, %.3f fps, %.3f s, %s%s", n, i.width, i.height, i.fps, i.duration, i.videoCodec.c_str(), i.isStill ? ", a still" : "");
    check(i.hasVideo && !i.hasAudio && i.isStill && i.duration == 0.0 && i.width == kW && i.height == kH, "%s is a %dx%d still picture with no duration", n, kW, kH);
    ImagePtr a = r->videoFrame(0.0, kW, kH), b = r->videoFrame(7.5, kW, kH), c = r->videoFrame(-3.0, kW, kH);
    check(a && b && c && frameNumber(a.get()) == number && a.get() == b.get() && a.get() == c.get(), "%s gives the same picture at any time", n);
    if (!a) return;
    const Mean bl = meanOf(*a, kBottomLeft, 4), tl = meanOf(*a, kTopLeft, 4);
    check(distance(bl, kOrange) <= 1.0 && distance(tl, kBlue) <= 1.0, "%s is the right way up%s", n,
          distance(tl, kOrange) <= 1.0 ? ": it is UPSIDE DOWN" : "");
    double worst = 0;
    for (int p = 0; p < kPatches; ++p) worst = std::max(worst, distance(meanOf(*a, patchRect(p), 4), kPatch[p]));
    check(worst <= 1.0, "%s colours are exactly as written (worst off by %.2f)", n, worst);
    ImagePtr half = r->videoFrame(0.0, 320, 180);
    check(half && half->w == 320 && half->h == 180 && frameNumber(half.get()) == number, "%s scales to 320x180", n);
}

// ---------------------------------------------------------------- any other files
void examine(const std::string& path)
{
    const std::string name = pathFileName(path);
    const char* n = name.c_str();
    std::string error;
    double t0 = nowMs();
    std::unique_ptr<MediaReader> r = openMedia(path, &error);
    if (!check(r != nullptr, "open %s (%.0f ms)%s%s", n, nowMs() - t0, r ? "" : ": ", r ? "" : error.c_str())) return;
    const MediaInfo& i = r->info();
    note("%s is %dx%d, %.3f fps, %.3f s, video %s, audio %s %d Hz %d channels%s", n, i.width, i.height, i.fps, i.duration,
         i.hasVideo ? i.videoCodec.c_str() : "none", i.hasAudio ? i.audioCodec.c_str() : "none", i.sampleRate, i.channels, i.isStill ? ", a still" : "");
    if (i.hasVideo) {
        for (int k = 0; k < 5; ++k) {
            const double t = i.duration * (k + 0.5) / 5.0;
            t0 = nowMs();
            ImagePtr img = r->videoFrame(t, 1280, 720);
            const double ms = nowMs() - t0;
            check(img != nullptr, "%s frame at %.3f s%s (%.1f ms)", n, t, img ? strfmt(" is %dx%d", img->w, img->h).c_str() : " could not be decoded", ms);
        }
        if (!i.isStill && i.fps > 0) {
            // thirty frames in a row from the middle, for a playback speed
            const double from = i.duration * 0.5;
            int got = 0;
            r->videoFrame(from, 1280, 720);
            t0 = nowMs();
            for (int k = 1; k <= 30; ++k)
                if (r->videoFrame(from + k / i.fps, 1280, 720)) ++got;
            const double ms = nowMs() - t0;
            check(got == 30, "%s 30 frames in a row from %.3f s (%.1f ms a frame, %.0f fps)", n, from, ms / 30.0, ms > 0 ? 30000.0 / ms : 0.0);
            t0 = nowMs();
            ImagePtr first = r->videoFrame(0.0, 1280, 720);
            check(first != nullptr, "%s back to the first frame (%.1f ms)", n, nowMs() - t0);
        }
    }
    if (i.hasAudio) {
        const int rate = 48000;
        std::vector<float> l((size_t)rate), rr((size_t)rate);
        const int64_t start = (int64_t)(i.duration * 0.5 * rate);
        t0 = nowMs();
        const bool ok = r->audio(start, rate, rate, l.data(), rr.data());
        const double ms = nowMs() - t0;
        check(ok, "%s one second of sound from %.3f s (%.1f ms, peak %.3f left %.3f right)", n, start / (double)rate, ms, peak(l.data(), 0, rate), peak(rr.data(), 0, rate));
        // the same second again, after reading somewhere else: must be the same samples
        std::vector<float> l2((size_t)rate), r2((size_t)rate), skip(4800);
        r->audio(0, 4800, rate, skip.data(), skip.data());
        if (r->audio(start, rate, rate, l2.data(), r2.data())) {
            double worst = 0;
            for (int k = 0; k < rate; ++k) worst = std::max(worst, (double)std::fabs(l[(size_t)k] - l2[(size_t)k]));
            check(worst < 0.05, "%s the same second read again after a seek is the same sound (worst difference %.5f)", n, worst);
        } else {
            check(false, "%s sound reads again after a seek", n);
        }
    }
}

void makeDirectory(const std::string& dir)
{
    if (dir.empty() || dirExists(dir)) return;
#ifdef _WIN32
    _wmkdir(widen(dir).c_str());
#else
    mkdir(dir.c_str(), 0777);
#endif
}

int run(const std::vector<std::string>& args)
{
    if (args.size() < 2) {
        std::printf("usage: mediatest <out dir> [media file ...]\n");
        return 2;
    }
    const std::string dir = args[1];
    makeDirectory(dir);
    const std::string logPath = joinPath(dir, "mediatest_log.txt");
#ifdef _WIN32
    gLog = _wfopen(widen(logPath).c_str(), L"w");
#else
    gLog = std::fopen(logPath.c_str(), "w");
#endif
    if (!gLog) std::printf("(cannot write %s; carrying on without a log file)\n", logPath.c_str());

    note("media backend: %s", mediaBackendVersion().c_str());
    std::vector<float> left, right;
    makeSound(left, right);

    // How exact each container can be. MP4 and MOV keep time to the sample; Matroska keeps it in
    // milliseconds, so a seek can be half a millisecond out; AVI has no way of recording the delay of
    // its audio encoder (5 ms for AC-3, 25 ms for MP3), so its clicks are late by that much; WAV is exact.
    const Tolerance lossy = {3.0, 1, 0.03, 12.0}, matroska = {3.0, 48, 0.03, 12.0}, avi = {30.0, 1, 0.03, 12.0}, pcm = {0.05, 0, 0.0001, 12.0};
    Clip clips[] = {
        {"clip.mp4", true, true, lossy, false},      {"clip.mkv", true, true, matroska, false}, {"clip.avi", true, true, avi, false},
        {"clip.mov", true, true, lossy, false},      {"clip.wav", false, true, pcm, false},     {"clip_pictures_only.mp4", true, false, lossy, false},
    };
    for (Clip& c : clips) c.written = writeClip(joinPath(dir, c.file), c.video, c.audio, 0, kFrames, left, right);
    // A picture file: the same writer, given a single picture (frame 37 of the clip).
    const std::string stillPath = joinPath(dir, "still.bmp");
    const bool stillWritten = writeClip(stillPath, true, false, 37, 1, left, right);

    for (const Clip& c : clips) {
        if (c.written) verifyClip(joinPath(dir, c.file), c, left, right);
        else note("%s was not written, so it is not read back", c.file);
    }
    if (stillWritten) verifyStill(stillPath, 37);
    else note("still.bmp was not written, so it is not read back");

    for (size_t k = 2; k < args.size(); ++k) examine(args[k]);

    emit(strfmt("mediatest: %d passed, %d failed", gPassed, gFailed));
    if (gLog) std::fclose(gLog);
    gLog = nullptr;
    return gFailed == 0 ? 0 : 1;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(narrow(argv[i]));
    return run(args);
}
#else
int main(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
    return run(args);
}
#endif
