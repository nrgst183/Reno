// Tests hardware (graphics card) video decoding: the choice of API and of streams, the session limit, the fall back to
// software, and the reader's decisions about when to copy a picture off the card. Without a card these run on an
// emulated one (hw::setEmulation: a software decoder whose pictures are treated as being on the card, copied for real
// and counted). Nothing here says how fast a real card is, or whether a real driver takes the stream.
//
//   hwdecode_test <out dir>
//
// Makes its clips with the ffmpeg program (skipped, with a note, where there is none).
#include "../../src/media/hwdecode.h"
#include "../../src/media/media.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static bool same(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b || a->w != b->w || a->h != b->h) return false;
    for (int y = 0; y < a->h; ++y)
        if (std::memcmp(a->row(y), b->row(y), (size_t)a->w * 4) != 0) return false;
    return true;
}
static int maxDiff(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b || a->w != b->w || a->h != b->h) return 999;
    int worst = 0;
    for (int y = 0; y < a->h; ++y)
        for (int x = 0; x < a->w * 4; ++x) worst = std::max(worst, std::abs((int)a->row(y)[x] - (int)b->row(y)[x]));
    return worst;
}

static bool run(const std::string& cmd) { return std::system((cmd + " > /dev/null 2>&1").c_str()) == 0; }

static std::unique_ptr<MediaReader> openWith(const std::string& path, HwDecodeMode mode, const std::string& api = std::string())
{
    setHardwareDecoding(mode, api);
    std::string err;
    auto r = openMedia(path, &err);
    if (!r) std::printf("  cannot open %s: %s\n", path.c_str(), err.c_str());
    return r;
}

using hw::Platform;
static std::string names(const std::vector<AVHWDeviceType>& v)
{
    std::string s;
    for (AVHWDeviceType t : v) s += std::string(s.empty() ? "" : ",") + hw::typeName(t);
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: hwdecode_test <out dir>\n"); return 2; }
    const std::string dir = argv[1];
    run("mkdir -p '" + dir + "'");

    // ================================================================ choices that need no card
    {
        const std::vector<AVHWDeviceType> all = {AV_HWDEVICE_TYPE_VDPAU, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_DXVA2,
                                                 AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_QSV};
        check(names(hw::candidateTypes(HwDecodeMode::Auto, "", Platform::Windows, all)) == "d3d11va,dxva2,cuda", "Windows tries Direct3D 11, then DXVA2, then CUDA", names(hw::candidateTypes(HwDecodeMode::Auto, "", Platform::Windows, all)));
        check(names(hw::candidateTypes(HwDecodeMode::Auto, "", Platform::Linux, all)) == "vaapi,vdpau,cuda", "Linux tries VAAPI, then VDPAU, then CUDA");
        check(names(hw::candidateTypes(HwDecodeMode::Auto, "", Platform::Mac, all)) == "videotoolbox", "macOS uses VideoToolbox");
        check(hw::candidateTypes(HwDecodeMode::Off, "", Platform::Windows, all).empty(), "Off gives no API");
        check(names(hw::candidateTypes(HwDecodeMode::Auto, "dxva2", Platform::Windows, all)) == "dxva2", "a named API is the only one tried");
        check(hw::candidateTypes(HwDecodeMode::Auto, "nonsense", Platform::Windows, all).empty(), "an unknown API name gives none");
        check(hw::candidateTypes(HwDecodeMode::Auto, "d3d11va", Platform::Windows, {AV_HWDEVICE_TYPE_VAAPI}).empty(), "an API this FFmpeg lacks is dropped");
        check(names(hw::candidateTypes(HwDecodeMode::Auto, "", Platform::Windows, {AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_CUDA})) == "dxva2,cuda", "APIs this FFmpeg lacks are skipped in the order");
        check(hw::candidateTypes(HwDecodeMode::Auto, "", Platform::Other, all).empty(), "an unknown platform tries nothing");

        std::string why;
        const int y420 = AV_PIX_FMT_YUV420P, y10 = AV_PIX_FMT_YUV420P10LE;
        check(hw::codecEligible(AV_CODEC_ID_H264, 100, y420, 1920, 1080, &why), "H.264 High 8-bit 4:2:0 goes to the card");
        check(!hw::codecEligible(AV_CODEC_ID_H264, 110, y10, 1920, 1080, &why), "H.264 10-bit does not", why);
        check(!hw::codecEligible(AV_CODEC_ID_H264, 100, y10, 1920, 1080, &why), "10-bit H.264 by its pixel format does not", why);
        check(!hw::codecEligible(AV_CODEC_ID_H264, 122, AV_PIX_FMT_YUV422P, 1920, 1080, &why), "H.264 4:2:2 does not", why);
        check(!hw::codecEligible(AV_CODEC_ID_H264, 100, AV_PIX_FMT_YUV444P, 1920, 1080, &why), "H.264 4:4:4 does not");
        check(hw::codecEligible(AV_CODEC_ID_HEVC, 2, y10, 3840, 2160), "HEVC Main 10 goes to the card");
        check(!hw::codecEligible(AV_CODEC_ID_HEVC, 4, AV_PIX_FMT_YUV420P12LE, 1920, 1080), "HEVC 12-bit range extensions do not");
        check(hw::codecEligible(AV_CODEC_ID_VP9, 0, y420, 1280, 720) && hw::codecEligible(AV_CODEC_ID_VP9, 2, y10, 1280, 720), "VP9 profiles 0 and 2 go");
        check(!hw::codecEligible(AV_CODEC_ID_VP9, 1, AV_PIX_FMT_YUV444P, 1280, 720), "VP9 profile 1 does not");
        check(hw::codecEligible(AV_CODEC_ID_AV1, 0, y420, 1920, 1080) && !hw::codecEligible(AV_CODEC_ID_AV1, 1, AV_PIX_FMT_YUV444P, 1920, 1080), "AV1 main goes, 4:4:4 does not");
        check(hw::codecEligible(AV_CODEC_ID_MPEG2VIDEO, 4, y420, 720, 576) && !hw::codecEligible(AV_CODEC_ID_MPEG2VIDEO, 0, AV_PIX_FMT_YUV422P, 720, 608), "MPEG-2 main goes, 4:2:2 does not");
        check(!hw::codecEligible(AV_CODEC_ID_PRORES, -99, AV_PIX_FMT_YUV422P10LE, 1920, 1080) && !hw::codecEligible(AV_CODEC_ID_MJPEG, -99, AV_PIX_FMT_YUVJ420P, 1920, 1080), "codecs with no card decoder stay in software");
        check(!hw::codecEligible(AV_CODEC_ID_H264, 100, y420, 32, 32), "a tiny picture stays in software");
        check(!hw::codecEligible(AV_CODEC_ID_H264, 100, AV_PIX_FMT_YUVA420P, 1920, 1080), "a picture with transparency stays in software");
        check(hw::codecEligible(AV_CODEC_ID_H264, 100, -1, 1920, 1080), "a stream whose pixel format is not known yet is tried");

        // settings
        setHardwareDecoding(HwDecodeMode::Off, "");
        check(hardwareDecoding() == HwDecodeMode::Off, "setting: off");
        setHardwareDecoding(HwDecodeMode::Auto, "vaapi");
        check(hardwareDecoding() == HwDecodeMode::Auto && hardwareDecodingApi() == "vaapi", "setting: auto with an API");
        setHardwareDecoding(HwDecodeMode::Auto, "auto");
        check(hardwareDecodingApi().empty(), "setting: \"auto\" as the API is the usual order");
        setHardwareDecoderLimit(0);
        check(hardwareDecoderLimit() == 1, "the session limit is at least 1");
        setHardwareDecoderLimit(3);
        {
            auto a = hw::acquireSession(), b = hw::acquireSession(), c = hw::acquireSession(), d = hw::acquireSession();
            check(a && b && c && !d && hw::activeSessions() == 3, "the fourth session over a limit of three is refused");
            b.reset();
            auto e = hw::acquireSession();
            check(e && hw::activeSessions() == 3, "a freed session can be taken again");
        }
        check(hw::activeSessions() == 0, "all sessions are given back");
        setHardwareDecoderLimit(8);
        std::string apis;
        for (const std::string& a : hardwareDecodeApis()) apis += a + " ";
        std::printf("  note: this FFmpeg's APIs for this system: %s\n", apis.empty() ? "(none)" : apis.c_str());
    }

    if (!run("ffmpeg -version")) {
        std::printf("NOTE: no ffmpeg program here to make clips; the rest is skipped\n");
        std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
        return gFail ? 1 : 0;
    }

    // ================================================================ clips
    const std::string src = "-f lavfi -i testsrc2=size=640x360:rate=30:duration=8";
    const std::string h264 = dir + "/hw_h264.mp4", h264b = dir + "/hw_h264_10bit.mp4", h264c = dir + "/hw_h264_422.mp4", hevc = dir + "/hw_hevc.mp4",
                      rot = dir + "/hw_rot.mp4", nv12 = dir + "/hw_nv12.nut", i420 = dir + "/hw_i420.nut", hevc10 = dir + "/hw_hevc10.mp4";
    const std::string x264 = " -c:v libx264 -preset veryfast -x264-params keyint=120:min-keyint=120:scenecut=0:bframes=3 ";
    bool made = true;
    made &= run("ffmpeg -v error -y " + src + x264 + "-pix_fmt yuv420p '" + h264 + "'");
    made &= run("ffmpeg -v error -y " + src + x264 + "-pix_fmt yuv420p10le '" + h264b + "'");
    made &= run("ffmpeg -v error -y " + src + x264 + "-pix_fmt yuv422p '" + h264c + "'");
    made &= run("ffmpeg -v error -y -display_rotation 90 " + src + x264 + "-pix_fmt yuv420p '" + rot + "'");
    const bool haveHevc = run("ffmpeg -v error -y " + src + " -c:v libx265 -preset ultrafast -x265-params keyint=120:log-level=none -pix_fmt yuv420p '" + hevc + "'");
    // the same pictures as raw NV12 / P010 and as raw planar, for the conversion of what a card hands over
    const std::string src2 = "-f lavfi -i testsrc2=size=640x360:rate=30:duration=2";
    made &= run("ffmpeg -v error -y " + src2 + " -c:v rawvideo -pix_fmt yuv420p '" + i420 + "'");
    made &= run("ffmpeg -v error -y -i '" + i420 + "' -c:v rawvideo -pix_fmt nv12 '" + nv12 + "'");        // (a repack, nothing lost)
    const bool haveHevc10 = run("ffmpeg -v error -y " + src + " -c:v libx265 -preset ultrafast -x265-params keyint=120:log-level=none -pix_fmt yuv420p10le '" + hevc10 + "'");
    check(made, "the test clips were made");
    if (!made) { std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail); return 1; }

    const int W = 640, H = 360;
    auto stamps = [](int n, unsigned seed, double dur) {
        std::mt19937 rng(seed);
        std::vector<double> t;
        for (int i = 0; i < n; ++i) t.push_back(std::uniform_real_distribution<double>(0.0, dur)(rng));
        return t;
    };

    // ================================================================ (a) hardware asked for, no card: software, same pictures
    {
        hw::setEmulation(false);
        const std::vector<double> times = stamps(14, 5, 7.9);
        auto ref = openWith(h264, HwDecodeMode::Off);
        std::vector<ImagePtr> want;
        for (double t : times) want.push_back(ref->videoFrame(t, W, H));
        check(ref->hardwareDecoder().empty(), "off: the reader decodes in software");
        for (const char* api : {"", "d3d11va", "dxva2", "cuda", "vaapi", "vdpau", "videotoolbox", "nonsense"}) {
            auto r = openWith(h264, HwDecodeMode::Auto, api);
            bool ok = r != nullptr;
            for (size_t i = 0; ok && i < times.size(); ++i) ok = same(r->videoFrame(times[i], W, H), want[i]);
            check(ok, std::string("hardware asked for (") + (*api ? api : "any") + "), no usable device: the same pictures as software");
            check(r && (r->hardwareDecoder().empty() || r->hardwareDecoder() == api || !*api), std::string("... and it says so (") + (r ? r->hardwareDecoder() : "?") + ")");
        }
        check(hw::activeSessions() == 0, "no session is left taken by readers that fell back");
        std::printf("  note: on this machine every real API failed to make a device, so the above is the silent fall back to software\n");
    }

    // ================================================================ (b) an emulated card
    {
        const std::vector<double> times = stamps(30, 11, 7.9);
        auto ref = openWith(h264, HwDecodeMode::Off);
        std::vector<ImagePtr> want;
        for (double t : times) want.push_back(ref->videoFrame(t, W, H));

        hw::setEmulation(true);
        hw::resetCounters();
        auto r = openWith(h264, HwDecodeMode::Auto);
        check(r && r->hardwareDecoder() == "emulated", "emulated card: the reader uses it");
        bool ok = true;
        for (size_t i = 0; i < times.size(); ++i) ok = ok && same(r->videoFrame(times[i], W, H), want[i]);
        check(ok, "emulated card: 30 random pictures (forwards, backwards, seeks) are the software pictures");
        check(hw::transfers() > 0, "emulated card: pictures were copied off it", std::to_string(hw::transfers()));
        r.reset();
        check(hw::activeSessions() == 0, "emulated card: closing the reader gives its session back");

        {   // a long way in from the start: only the pictures kept or returned are copied
            hw::resetCounters();
            auto far = openWith(h264, HwDecodeMode::Auto);
            auto farRef = openWith(h264, HwDecodeMode::Off);
            ImagePtr a = far->videoFrame(7.0, W, H);   // (picture 210: the key frame is at 120, 90 pictures before)
            const long copied = hw::transfers();
            check(same(a, farRef->videoFrame(7.0, W, H)), "emulated card: a picture far past a key frame is the software one");
            check(copied > 0 && copied < 30, "emulated card: seeking through a group of pictures copies only the last stretch", std::to_string(copied) + " copies for ~90 decoded");
            // ... and the stretch kept lets a step back find its picture without decoding again
            hw::resetCounters();
            check(same(far->videoFrame(6.9, W, H), farRef->videoFrame(6.9, W, H)) && same(far->videoFrame(6.8, W, H), farRef->videoFrame(6.8, W, H)), "emulated card: stepping back a few pictures after it");
        }
        {   // playing on: every picture decoded is returned
            hw::resetCounters();
            auto seq = openWith(h264, HwDecodeMode::Auto);
            auto seqRef = openWith(h264, HwDecodeMode::Off);
            bool seqOk = true;
            for (int f = 0; f < 60; ++f) seqOk = seqOk && same(seq->videoFrame(f / 30.0, W, H), seqRef->videoFrame(f / 30.0, W, H));
            check(seqOk, "emulated card: 60 pictures in a row are the software pictures");
            check(hw::transfers() >= 60 && hw::transfers() <= 66, "emulated card: playing on copies each picture once", std::to_string(hw::transfers()));
        }
        {   // backwards: all the stretch decoded is kept
            auto bk = openWith(h264, HwDecodeMode::Auto);
            auto bkRef = openWith(h264, HwDecodeMode::Off);
            bool bkOk = true;
            for (int f = 150; f >= 100; --f) bkOk = bkOk && same(bk->videoFrame(f / 30.0, W, H), bkRef->videoFrame(f / 30.0, W, H));
            check(bkOk, "emulated card: 50 pictures backwards are the software pictures");
        }

        // streams the card cannot take stay in software
        for (const std::string* f : {&h264b, &h264c}) {
            auto s = openWith(*f, HwDecodeMode::Auto);
            auto sRef = openWith(*f, HwDecodeMode::Off);
            check(s && s->hardwareDecoder().empty() && same(s->videoFrame(1.0, W, H), sRef->videoFrame(1.0, W, H)), "10-bit / 4:2:2 H.264 stays in software with the same pictures");
        }
        if (haveHevc) {
            auto s = openWith(hevc, HwDecodeMode::Auto);
            auto sRef = openWith(hevc, HwDecodeMode::Off);
            bool good = s && s->hardwareDecoder() == "emulated";
            for (double t : {0.0, 3.3, 7.5, 1.2}) good = good && same(s->videoFrame(t, W, H), sRef->videoFrame(t, W, H));
            check(good, "emulated card: HEVC");
        }
        {   // rotation, small output, quick reads
            auto s = openWith(rot, HwDecodeMode::Auto);
            auto sRef = openWith(rot, HwDecodeMode::Off);
            check(s && s->hardwareDecoder() == "emulated" && s->info().width == 360 && same(s->videoFrame(2.5, 200, 200), sRef->videoFrame(2.5, 200, 200)), "emulated card: a rotated file at a smaller size");
            auto q = openWith(h264, HwDecodeMode::Auto);
            auto qRef = openWith(h264, HwDecodeMode::Off);
            ImagePtr qa, qb;
            bool approxA = false, approxB = false;
            {   MediaReadHintsScope h(nullptr, true); qa = q->videoFrame(6.0, W, H); approxA = h.approximated(); }
            {   MediaReadHintsScope h(nullptr, true); qb = qRef->videoFrame(6.0, W, H); approxB = h.approximated(); }
            check(approxA && approxB && same(qa, qb), "emulated card: a quick read is the same nearest picture");
            check(same(q->videoFrame(6.0, W, H), qRef->videoFrame(6.0, W, H)), "emulated card: the exact picture after a quick one");
            std::atomic<bool> stop{true};
            ImagePtr none;
            { MediaReadHintsScope h(&stop, false); none = q->videoFrame(3.0, W, H); }
            check(!none, "emulated card: a cancelled read gives nothing");
            check(same(q->videoFrame(3.0, W, H), qRef->videoFrame(3.0, W, H)) && same(q->videoFrame(2.9, W, H), qRef->videoFrame(2.9, W, H)), "emulated card: reads after a cancel are right, forwards and back");
        }
        {   // deep pictures straight to float, and the 8-bit way
            auto d = openWith(h264, HwDecodeMode::Auto);
            auto dRef = openWith(h264, HwDecodeMode::Off);
            ImagePtr fa = d->videoFrameInDepth(2.0, W, H, PixelDepth::Float), fb = dRef->videoFrameInDepth(2.0, W, H, PixelDepth::Float);
            check(fa && fb && fa->w == fb->w && fa->depth == fb->depth, "emulated card: depth read");
        }

        // ---- the card fails half way: software takes over from the same picture, whatever was being done
        for (int failAfter : {1, 3, 20, 61}) {
            for (int mode = 0; mode < 2; ++mode) {
                hw::setEmulation(true, mode == 0 ? failAfter : -1, mode == 1 ? failAfter : -1);
                auto f = openWith(h264, HwDecodeMode::Auto);
                bool good = f && f->hardwareDecoder() == "emulated";
                // forwards one by one, then a jump, then backwards, then another jump
                std::vector<double> plan;
                for (int k = 0; k < 90; ++k) plan.push_back(k / 30.0);
                for (double t : {6.2, 6.1, 6.0, 5.9, 2.0, 7.7}) plan.push_back(t);
                for (double t : plan) good = good && same(f->videoFrame(t, W, H), ref->videoFrame(t, W, H));
                check(good, strfmt("the card fails %s picture %d: every picture is still the software one", mode == 0 ? "copying" : "decoding", failAfter));
                check(f && f->hardwareDecoder().empty(), strfmt("... and the reader is in software afterwards (%s)", f ? f->hardwareDecoder().c_str() : "?"));
                f.reset();
                check(hw::activeSessions() == 0, "... with its session given back");
            }
        }
        {   // a failure while the last picture is asked for twice (the copy happens in videoFrame, not in decoding)
            hw::setEmulation(true, 0);          // the very first copy fails
            auto f = openWith(h264, HwDecodeMode::Auto);
            bool good = true;
            for (double t : {0.0, 0.5, 4.0, 3.0}) good = good && same(f->videoFrame(t, W, H), ref->videoFrame(t, W, H));
            check(good && f->hardwareDecoder().empty(), "the card fails at the very first picture");
        }

        // ---- many readers of one file at once: the limit
        hw::setEmulation(true);
        setHardwareDecoderLimit(3);
        std::vector<std::unique_ptr<MediaReader>> many;
        int onCard = 0;
        for (int i = 0; i < 7; ++i) {
            many.push_back(openWith(h264, HwDecodeMode::Auto));
            if (many.back() && !many.back()->hardwareDecoder().empty()) ++onCard;
        }
        check(onCard == 3 && hw::activeSessions() == 3, "seven readers of one file: three on the card, four in software", std::to_string(onCard));
        bool allSame = true;
        for (auto& m : many) allSame = allSame && same(m->videoFrame(3.0, W, H), ref->videoFrame(3.0, W, H));
        check(allSame, "... and all of them show the same picture");
        many.erase(many.begin());           // the first one (on the card) goes
        auto again = openWith(h264, HwDecodeMode::Auto);
        check(again && again->hardwareDecoder() == "emulated", "a place freed by a closed reader goes to the next one opened");
        many.clear();
        again.reset();
        check(hw::activeSessions() == 0, "all sessions are given back when the readers are gone");
        setHardwareDecoderLimit(8);

        // ---- off means off
        auto offr = openWith(h264, HwDecodeMode::Off);
        check(offr && offr->hardwareDecoder().empty(), "off, with a card at hand: software");
        hw::setEmulation(false);
    }

    // ================================================================ (c) what a card hands over: NV12 and P010 to the converter
    {
        auto a = openWith(nv12, HwDecodeMode::Off), b = openWith(i420, HwDecodeMode::Off);
        check(a && b && a->info().width == W, "raw NV12 and planar clips open");
        if (a && b) {
            int worst = 0;
            for (double t : {0.0, 0.5, 1.0, 1.9}) worst = std::max(worst, maxDiff(a->videoFrame(t, W, H), b->videoFrame(t, W, H)));
            // (swscale rounds chroma differently in its semi-planar path, at colour edges only: close, not equal - which is why
            // what a card hands over is repacked to planar before it gets there)
            double sum = 0;
            ImagePtr x = a->videoFrame(1.0, W, H), y = b->videoFrame(1.0, W, H);
            for (int r = 0; x && y && r < x->h; ++r)
                for (int i = 0; i < x->w * 4; ++i) sum += std::abs((int)x->row(r)[i] - (int)y->row(r)[i]);
            const double mean = x ? sum / ((double)x->w * x->h * 4) : 99;
            check(mean < 1.0, "NV12 pictures from a file convert (colours right: mean difference from planar 4:2:0 under 1)", std::to_string(mean));
        }
        // the emulated card hands the pictures over as NV12 (8-bit) and P010 (10-bit), as a real one does
        hw::setEmulation(true, -1, -1, true);
        {
            auto e = openWith(h264, HwDecodeMode::Auto), eRef = openWith(h264, HwDecodeMode::Off);
            int worst = e && e->hardwareDecoder() == "emulated" ? 0 : 999;
            for (double t : {0.0, 1.0, 2.5, 6.0, 3.0}) worst = std::max(worst, maxDiff(e->videoFrame(t, W, H), eRef->videoFrame(t, W, H)));
            check(worst == 0, "emulated card handing over NV12: the same pictures as software planar", std::to_string(worst));
            worst = 0;
            for (double t : {0.5, 4.5}) worst = std::max(worst, maxDiff(e->videoFrame(t, 200, 100), eRef->videoFrame(t, 200, 100)));
            check(worst == 0, "... at a smaller size", std::to_string(worst));
        }
        if (haveHevc10) {
            auto e = openWith(hevc10, HwDecodeMode::Auto), eRef = openWith(hevc10, HwDecodeMode::Off);
            check(e && e->hardwareDecoder() == "emulated" && e->info().bitDepth == 10, "emulated card: 10-bit HEVC is taken");
            int worst = 0;
            for (double t : {0.0, 1.0, 2.5, 6.0, 3.0}) worst = std::max(worst, maxDiff(e->videoFrame(t, W, H), eRef->videoFrame(t, W, H)));
            check(worst == 0, "emulated card handing over P010: the same pictures as software planar 10-bit", std::to_string(worst));
            double fw = 0;
            for (double t : {1.0, 5.0}) {
                ImagePtr f1 = e->videoFrameInDepth(t, W, H, PixelDepth::Float), f2 = eRef->videoFrameInDepth(t, W, H, PixelDepth::Float);
                if (f1 && f2 && f1->w == f2->w && f1->h == f2->h && f1->depth == PixelDepth::Float)
                    for (int y = 0; y < f1->h; ++y)
                        for (int x = 0; x < f1->w * 4; ++x) fw = std::max(fw, (double)std::fabs(((const float*)f1->row(y))[x] - ((const float*)f2->row(y))[x]));
                else fw = 9;
            }
            check(fw == 0, "P010 pictures to float: the same as software planar 10-bit", std::to_string(fw));
        }
        hw::setEmulation(false);
    }

    setHardwareDecoding(HwDecodeMode::Off, "");
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
