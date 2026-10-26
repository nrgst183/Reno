// Tests of how the preview's media pool decodes a clip when several frame threads play it (MediaPool::setShareFrames):
// each frame is decoded about once, repeated stretches come from the shared frames, a waiting read stops when
// cancelled, and the frames are the same shared or not.
//
//   clipshare_test
#include "../../src/engine/engine.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static std::atomic<int> gDecoded{0};            // frames decoded by all the readers
static std::atomic<int> gSlowFrame{-1};         // a frame whose decoding takes long (the cancel test)
static std::atomic<int> gSlowMs{0};

// 30 fps, a key frame every 30 frames. Reads on decode one frame at a time (1 ms each); anything else seeks back to the key
// frame and decodes up from there. The picture's blue is the frame number.
class Clip : public MediaReader {
public:
    MediaInfo mi;
    int cur = -1000;
    std::vector<int> pending;                   // frames decoded on the way to the one given, not handed out yet (harvestKept)
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int, int) override { double a, b; return videoFrameSpan(seconds, 16, 16, &a, &b); }
    ImagePtr videoFrameSpan(double seconds, int, int, double* from, double* until) override
    {
        const int n = std::max(0, std::min(2999, (int)std::floor(seconds * 30.0 + 1e-6)));
        if (!(n >= cur && n - cur < 40)) cur = n / 30 * 30 - 1;
        while (cur < n) {
            ++cur;
            ++gDecoded;
            pending.push_back(cur);
            std::this_thread::sleep_for(std::chrono::milliseconds(cur == gSlowFrame.load() ? gSlowMs.load() : 1));
        }
        *from = n / 30.0;
        *until = (n + 1) / 30.0;
        pending.erase(std::remove(pending.begin(), pending.end(), n), pending.end());
        return make(n);
    }
    static ImagePtr make(int n)
    {
        ImagePtr img = Image::make(16, 16, PixelDepth::Byte, false);
        for (int y = 0; y < 16; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < 16; ++x, d += 4) { d[0] = (uint8_t)(n & 255); d[1] = (uint8_t)(n >> 8); d[2] = 0; d[3] = 255; }
        }
        return img;
    }
    int harvestKept(int, int, int most, const std::function<void(double, double, const ImagePtr&)>& put) override
    {
        int done = 0;
        for (size_t k = pending.size(); k-- > 0 && done < most; ++done) put(pending[k] / 30.0, (pending[k] + 1) / 30.0, make(pending[k]));
        pending.clear();
        return done;
    }
    int videoCost(double seconds, int, int) override
    {
        const int n = std::max(0, std::min(2999, (int)std::floor(seconds * 30.0 + 1e-6)));
        return n == cur ? 0 : (n > cur && n - cur < 40) ? 1 : 2;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> openClip(const std::string& path, std::string*)
{
    std::unique_ptr<Clip> r(new Clip());
    r->mi.path = path;
    r->mi.hasVideo = true;
    r->mi.width = 16;
    r->mi.height = 16;
    r->mi.fps = 30;
    r->mi.duration = 100;
    return std::unique_ptr<MediaReader>(r.release());
}

static MediaItem item(uint64_t id)
{
    MediaItem m;
    m.id = id;
    m.path = "clip" + std::to_string(id);
    m.name = m.path;
    m.info = openClip(m.path, nullptr)->info();
    return m;
}

static int blueOf(const ImagePtr& img) { return img ? img->row(0)[0] + 256 * img->row(0)[1] : -1; }

// 'threads' threads play frames from..from+count of the clip, frame k going to thread k mod threads (as the frame threads take
// turns), each at about the pace of a real time play: a frame every 'periodMs' for the set as a whole.
static int playFrames(MediaPool& pool, const MediaItem& m, int from, int count, int threads, int& wrong)
{
    gDecoded = 0;
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            for (int k = t; k < count; k += threads) {
                const int n = from + k;
                ImagePtr img = pool.videoFrame(1000 + t, m, n / 30.0 + 0.001, 16, 16);
                if (blueOf(img) != (n & 255) + 256 * (n >> 8)) ++bad;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    for (std::thread& t : ts) t.join();
    wrong = bad;
    return gDecoded;
}

int main()
{
    const MediaItem m = item(1);
    const int frames = 240;
    int wrongShared = 0, wrongPlain = 0;

    MediaPool plain;
    plain.setOpener(openClip);
    const int decodedPlain = playFrames(plain, m, 300, frames, 4, wrongPlain);

    MediaPool shared;
    shared.setOpener(openClip);
    shared.setShareFrames(true);
    const int decodedShared = playFrames(shared, m, 300, frames, 4, wrongShared);
    std::printf("decoded for %d frames by 4 threads: %d without sharing, %d with (shared %llu, waited %llu, seeks %llu)\n", frames, decodedPlain,
                decodedShared, (unsigned long long)shared.stats().videoShared, (unsigned long long)shared.stats().videoWaited,
                (unsigned long long)shared.stats().videoSeeks);
    check(wrongPlain == 0 && wrongShared == 0, "frames given are the clip's frames at those times", "wrong " + std::to_string(wrongPlain) + "/" + std::to_string(wrongShared));
    check(decodedShared <= frames + 3 * 30, "4 threads decode each frame of a clip about once", std::to_string(decodedShared) + " decoded for " + std::to_string(frames));
    check(decodedShared < decodedPlain || decodedPlain <= frames + 90, "sharing never decodes more than not sharing", std::to_string(decodedShared) + " vs " + std::to_string(decodedPlain));

    // The same stretch again (a repeated piece): from the shared frames, nothing decoded.
    int wrong = 0;
    const int again = playFrames(shared, m, 300, 90, 4, wrong);
    check(again == 0 && wrong == 0, "a stretch played again is given from the shared frames", std::to_string(again) + " decoded, " + std::to_string(wrong) + " wrong");

    // Cancelling: thread A's reader is busy on a slow frame; thread B asks for one a little on and waits for it; B's read is cancelled
    // (the play position went on to another clip) and stops at once, decoding nothing more for the old clip.
    MediaPool cp;
    cp.setOpener(openClip);
    cp.setShareFrames(true);
    const MediaItem m2 = item(2);
    cp.videoFrame(1, m2, 0.001, 16, 16);                    // (the reader is open and in place)
    gSlowFrame = 10;
    gSlowMs = 600;
    gDecoded = 0;
    std::thread a([&] { cp.videoFrame(1, m2, 10 / 30.0 + 0.001, 16, 16); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    std::atomic<bool> cancel(false);
    std::atomic<double> tookMs(-1);
    ImagePtr got;
    std::thread b([&] {
        MediaReadHintsScope hints(&cancel, false);
        const auto t0 = std::chrono::steady_clock::now();
        got = cp.videoFrame(2, m2, 14 / 30.0 + 0.001, 16, 16);
        tookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const bool waiting = tookMs.load() < 0;
    cancel = true;
    b.join();
    const double took = tookMs.load();
    a.join();
    gSlowFrame = -1;
    check(waiting, "a read just ahead of a busy reader waits for it");
    check(!got && took < 250, "a cancelled read stops waiting at once", std::to_string(took) + " ms");
    check(cp.stats().videoOpened == 1, "no second reader is opened for the stretch the busy one is decoding", std::to_string(cp.stats().videoOpened) + " opened");

    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
