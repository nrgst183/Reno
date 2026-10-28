// Smart rendering: a render copies whole closed groups of pictures of its source file as they are when the codec and the
// picture match, and encodes the rest; anything done to the picture (an effect, a fade, another size) copies nothing.
//
//   smartrender_test <output folder>
#include "../../src/engine/engine.h"
#include "../../src/engine/renderas.h"
#include "../../src/engine/smartplan.h"
#include "../../src/media/ffmpeg_common.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const int kW = 128, kH = 72, kFrames = 60;

static EncodeSettings baseSettings(const std::string& path, const std::string& family, int bframes)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = kW;
    es.height = kH;
    es.fpsNum = 25;
    es.fpsDen = 1;
    es.videoFamily = family;
    es.rateControl = "quality";
    es.quality = 20;
    es.keyframeSeconds = 0.4;       // a group every 10 frames
    es.bFrames = bframes;
    if (family == "hevc") es.encoderOptions = "x265-params=open-gop=0";        // x265's groups are open by default: nothing to copy
    return es;
}

static std::string makeSource(const EncodeSettings& es)
{
    std::string err;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
    if (!w) return "cannot write " + es.path + ": " + err;
    ImagePtr img = Image::make(kW, kH, PixelDepth::Byte, true);
    for (int f = 0; f < kFrames; ++f) {
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                uint8_t* p = img->row(y) + x * 4;
                p[0] = (uint8_t)((x * 2 + f * 5) & 255);
                p[1] = (uint8_t)((y * 3 + f * 3) & 255);
                p[2] = (uint8_t)(((x ^ y) + f * 7) & 255);
                p[3] = 255;
            }
        if (!w->writeVideo(*img)) return "cannot write a frame";
    }
    if (!w->finish(&err)) return "cannot finish " + es.path + ": " + err;
    return std::string();
}

// All the video packets of a file, in the order they are stored.
static std::vector<std::string> packetsOf(const std::string& path, int* keys = nullptr)
{
    std::vector<std::string> out;
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return out;
    avformat_find_stream_info(fc, nullptr);
    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket* pk = av_packet_alloc();
    while (vi >= 0 && av_read_frame(fc, pk) >= 0) {
        if (pk->stream_index == vi) {
            out.emplace_back((const char*)pk->data, (size_t)pk->size);
            if (keys && (pk->flags & AV_PKT_FLAG_KEY)) ++*keys;
        }
        av_packet_unref(pk);
    }
    av_packet_free(&pk);
    avformat_close_input(&fc);
    return out;
}

static MediaItem mediaOf(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    std::string err;
    std::unique_ptr<MediaReader> r = openMedia(path, &err);
    if (r) m.info = r->info();
    p.media.push_back(m);
    return m;
}

static Project projectOf(const std::string& clip, double offset, double length)
{
    Project p;
    p.settings.width = kW;
    p.settings.height = kH;
    p.settings.fpsNum = 25;
    p.settings.fpsDen = 1;
    MediaItem v = mediaOf(p, clip);
    Track vt;
    vt.id = p.newId();
    vt.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = v.id;
    e.start = 0;
    e.offset = offset;
    e.length = length;
    vt.events.push_back(e);
    p.tracks = {vt};
    return p;
}

static int copiedOf(const std::string& description)
{
    const size_t at = description.find("smart render: ");
    int n = 0;
    if (at != std::string::npos && std::sscanf(description.c_str() + at + 14, "%d of", &n) == 1) return n;
    return 0;
}

// Pictures of the two files at frame a of one and b of the other are the same.
static bool samePicture(const std::string& fa, int a, const std::string& fb, int b)
{
    std::string e;
    std::unique_ptr<MediaReader> ra = openMedia(fa, &e), rb = openMedia(fb, &e);
    if (!ra || !rb) return false;
    ImagePtr ia = ra->videoFrame((a + 0.5) / 25.0, kW, kH), ib = rb->videoFrame((b + 0.5) / 25.0, kW, kH);
    if (!ia || !ib || ia->w != ib->w || ia->h != ib->h) return false;
    for (int y = 0; y < ia->h; ++y)
        if (std::memcmp(ia->row(y), ib->row(y), (size_t)ia->w * 4) != 0) return false;
    return true;
}


// Decodes a whole file with every error fatal; the frames got, and how the video packets' times run.
struct StreamFacts {
    int frames = 0, errors = 0;
    bool dtsRising = true, ptsEvenly = true, opened = false;
    double videoStart = 0, videoEnd = 0, audioStart = -1, audioEnd = -1;
};
static StreamFacts factsOf(const std::string& path)
{
    StreamFacts f;
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return f;
    avformat_find_stream_info(fc, nullptr);
    const int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vi < 0) { avformat_close_input(&fc); return f; }
    const AVCodec* dec = avcodec_find_decoder(fc->streams[vi]->codecpar->codec_id);
    AVCodecContext* cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, fc->streams[vi]->codecpar);
    cc->err_recognition = AV_EF_EXPLODE;
    f.opened = avcodec_open2(cc, dec, nullptr) >= 0;
    AVPacket* pk = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    std::vector<int64_t> pts;
    int64_t lastDts = INT64_MIN;
    const AVRational tb = fc->streams[vi]->time_base;
    auto drain = [&]() {
        for (;;) {
            const int r = avcodec_receive_frame(cc, fr);
            if (r == 0) { ++f.frames; av_frame_unref(fr); continue; }
            if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) ++f.errors;
            break;
        }
    };
    while (f.opened && av_read_frame(fc, pk) >= 0) {
        if (pk->stream_index == vi) {
            if (pk->dts != AV_NOPTS_VALUE) {
                if (pk->dts <= lastDts) f.dtsRising = false;
                lastDts = pk->dts;
            }
            pts.push_back(av_rescale_q(pk->pts, tb, av_make_q(1, 25)));
            if (avcodec_send_packet(cc, pk) < 0) ++f.errors;
            drain();
        } else if (pk->stream_index == ai && ai >= 0) {
            const AVStream* st = fc->streams[ai];
            const double t0 = pk->pts * av_q2d(st->time_base), t1 = (pk->pts + pk->duration) * av_q2d(st->time_base);
            if (f.audioStart < 0) f.audioStart = t0;
            f.audioEnd = t1;
        }
        av_packet_unref(pk);
    }
    if (f.opened) { avcodec_send_packet(cc, nullptr); drain(); }
    std::sort(pts.begin(), pts.end());
    for (size_t k = 0; k < pts.size(); ++k)
        if (pts[k] != pts[0] + (int64_t)k) f.ptsEvenly = false;
    f.videoStart = pts.empty() ? 0 : (double)pts[0] / 25.0;
    f.videoEnd = (double)(pts.empty() ? 0 : pts.back() + 1) / 25.0;
    av_frame_free(&fr);
    av_packet_free(&pk);
    avcodec_free_context(&cc);
    avformat_close_input(&fc);
    return f;
}

// The picture of frame a of one file against frame b of another, as PSNR in dB over the colour bytes (99: identical).
static double psnr(const std::string& fa, int a, const std::string& fb, int b)
{
    std::string e;
    std::unique_ptr<MediaReader> ra = openMedia(fa, &e), rb = openMedia(fb, &e);
    if (!ra || !rb) return 0;
    ImagePtr ia = ra->videoFrame((a + 0.5) / 25.0, kW, kH), ib = rb->videoFrame((b + 0.5) / 25.0, kW, kH);
    if (!ia || !ib || ia->w != ib->w || ia->h != ib->h) return 0;
    double sum = 0;
    for (int y = 0; y < ia->h; ++y)
        for (int x = 0; x < ia->w * 4; ++x) {
            if ((x & 3) == 3) continue;
            const double d = (double)ia->row(y)[x] - ib->row(y)[x];
            sum += d * d;
        }
    if (sum == 0) return 99;
    return 10 * std::log10(255.0 * 255.0 / (sum / ((double)ia->h * ia->w * 3)));
}

static void testCodec(const std::string& out, const std::string& family, const std::string& ext, int bframes, bool intra)
{
    const std::string tag = family + (bframes > 0 ? " with B-frames" : "");
    const EncodeSettings src = baseSettings(pathJoin(out, "src_" + family + std::to_string(bframes) + "." + ext), family, bframes);
    const std::string se = makeSource(src);
    check(se.empty(), tag + ": the source is made", se);
    if (!se.empty()) return;
    const std::vector<std::string> sp = packetsOf(src.path);
    check((int)sp.size() == kFrames, tag + ": the source has all its frames", std::to_string(sp.size()));

    // an event that starts 5 frames into the file and runs 50: the groups 10..49 are whole inside it
    Project p = projectOf(src.path, 0.2, 2.0);
    FxRuntime fx;
    RenderSpec spec;
    spec.enc = baseSettings(pathJoin(out, "out_" + family + std::to_string(bframes) + "." + ext), family, bframes);
    spec.smartRender = true;
    std::string e, description;
    const bool ok = renderProjectAs(p, fx, spec, 0, 2.0, nullptr, &e, MediaPool::Opener(), &description);
    check(ok, tag + ": the smart render finishes", e);
    if (!ok) return;
    // the groups of the source that lie whole inside the event (frames 5 .. 54 of the file)
    std::shared_ptr<const SmartSource> info;
    std::string why;
    check(analyzeSmartSource(src.path, &info, &why), tag + ": the source can be looked at", why);
    if (!info) return;
    int expect = 0;
    std::vector<int> wholeFrames;
    for (const SmartGop& g : info->gops)
        if (g.startFrame >= 5 && g.startFrame + g.frames <= 55)
            for (int f = (int)g.startFrame; f < g.startFrame + g.frames; ++f) { ++expect; wholeFrames.push_back(f); }
    const int copied = copiedOf(description);
    check(copied == expect && expect >= (intra ? 50 : 20), tag + ": the whole groups inside the event are copied", description + strfmt(" (expected %d)", expect));
    const std::vector<std::string> op = packetsOf(spec.enc.path);
    check(op.size() == 50, tag + ": the file has 50 frames", std::to_string(op.size()));
    int found = 0;
    for (int f : wholeFrames)
        for (const std::string& o : op)
            if (o == sp[(size_t)f]) { ++found; break; }
    check(found == copied, tag + ": the copied packets are the source's, byte for byte", std::to_string(found));
    bool same = true;
    for (int f : wholeFrames) same = same && samePicture(src.path, f, spec.enc.path, f - 5);
    check(same, tag + ": the copied frames decode to the source's pictures");

    // the junctions: decodes cleanly with every error fatal, times run on without a gap or a repeat, and the pictures either side of
    // each cut are as good as an ordinary render's
    {
        const StreamFacts sf = factsOf(spec.enc.path);
        check(sf.opened && sf.errors == 0 && sf.frames == 50, tag + ": decodes with no error (errors fatal)", strfmt("%d frames, %d errors", sf.frames, sf.errors));
        check(sf.dtsRising && sf.ptsEvenly && sf.videoStart == 0, tag + ": decoding times rise and shown times have no gap or repeat");
    }
    {
        RenderSpec plain2 = spec;
        plain2.smartRender = false;
        plain2.enc.path = pathJoin(out, "ref_" + family + std::to_string(bframes) + "." + ext);
        std::string d3;
        renderProjectAs(p, fx, plain2, 0, 2.0, nullptr, &e, MediaPool::Opener(), &d3);
        bool good = !wholeFrames.empty();
        std::string worst;
        // frames either side of every junction of copied and encoded stretches (output frames)
        std::vector<int> around;
        for (size_t k = 0; k < wholeFrames.size(); ++k) {
            const int o = wholeFrames[k] - 5;
            const bool first = k == 0 || wholeFrames[k - 1] != wholeFrames[k] - 1, last = k + 1 == wholeFrames.size() || wholeFrames[k + 1] != wholeFrames[k] + 1;
            if (first) { around.push_back(o - 1); around.push_back(o); }
            if (last) { around.push_back(o); around.push_back(o + 1); }
        }
        for (int o : around) {
            if (o < 0 || o > 49) continue;
            const double q = psnr(spec.enc.path, o, plain2.enc.path, o);
            if (q < 30) { good = false; worst = strfmt("frame %d: %.1f dB", o, q); }
        }
        check(good, tag + ": the frames either side of each junction match an ordinary render", worst);
    }

    // the same render without smart rendering copies nothing
    RenderSpec plain = spec;
    plain.smartRender = false;
    plain.enc.path = pathJoin(out, "plain_" + family + std::to_string(bframes) + "." + ext);
    std::string d2;
    const bool ok2 = renderProjectAs(p, fx, plain, 0, 2.0, nullptr, &e, MediaPool::Opener(), &d2);
    check(ok2 && copiedOf(d2) == 0 && d2.find("smart") == std::string::npos, tag + ": without the option nothing is copied");
    const std::vector<std::string> pp = packetsOf(plain.enc.path);
    int same2 = 0;
    for (const std::string& o : pp)
        for (const std::string& s : sp)
            if (o == s) { ++same2; break; }
    check(same2 == 0 || intra, tag + ": an ordinary render makes new packets");
}


// Sound beside copied frames stays where an ordinary render puts it.
static void testAudio(const std::string& out)
{
    const EncodeSettings src = baseSettings(pathJoin(out, "au_src.mp4"), "h264", 0);
    check(makeSource(src).empty(), "audio: the source is made");
    const std::string wav = pathJoin(out, "au_tone.wav");
    {
        EncodeSettings es;
        es.path = wav;
        es.video = false;
        es.sampleRate = 48000;
        es.audioFamily = "pcm";
        std::string err;
        std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
        check(w != nullptr, "audio: the tone file is made", err);
        if (!w) return;
        std::vector<float> l(4800), r(4800);
        for (int b = 0; b < 30; ++b) {
            for (int k = 0; k < 4800; ++k) l[(size_t)k] = r[(size_t)k] = (float)(0.3 * std::sin(2 * 3.14159265 * 440.0 * (b * 4800 + k) / 48000.0));
            w->writeAudio(l.data(), r.data(), 4800);
        }
        w->finish(&err);
    }
    Project p = projectOf(src.path, 0.2, 2.0);
    MediaItem a = mediaOf(p, wav);
    Track at;
    at.id = p.newId();
    at.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = a.id;
    e.start = 0;
    e.length = 2.0;
    at.events.push_back(e);
    p.tracks.push_back(at);
    FxRuntime fx;
    RenderSpec spec;
    spec.enc = baseSettings(pathJoin(out, "au_smart.mp4"), "h264", 0);
    spec.enc.audio = true;
    spec.enc.audioFamily = "aac";
    spec.smartRender = true;
    RenderSpec plain = spec;
    plain.smartRender = false;
    plain.enc.path = pathJoin(out, "au_plain.mp4");
    std::string er, d1, d2;
    const bool ok1 = renderProjectAs(p, fx, spec, 0, 2.0, nullptr, &er, MediaPool::Opener(), &d1);
    const bool ok2 = renderProjectAs(p, fx, plain, 0, 2.0, nullptr, &er, MediaPool::Opener(), &d2);
    check(ok1 && ok2 && copiedOf(d1) > 0, "audio: both renders finish and the smart one copies", d1 + er);
    const StreamFacts s1 = factsOf(spec.enc.path), s2 = factsOf(plain.enc.path);
    check(s1.errors == 0 && s1.frames == 50 && s1.dtsRising && s1.ptsEvenly, "audio: the smart file decodes cleanly");
    check(std::fabs(s1.audioStart - s2.audioStart) < 0.025 && std::fabs(s1.audioEnd - s2.audioEnd) < 0.025 && std::fabs(s1.videoEnd - s2.videoEnd) < 0.001,
          "audio: sound and picture sit where an ordinary render puts them",
          strfmt("audio %.3f..%.3f against %.3f..%.3f, video ends %.3f against %.3f", s1.audioStart, s1.audioEnd, s2.audioStart, s2.audioEnd, s1.videoEnd, s2.videoEnd));
}

int main(int argc, char** argv)
{
    const std::string out = argc > 1 ? argv[1] : "smartout";
    std::system(("mkdir -p '" + out + "'").c_str());

    testCodec(out, "h264", "mp4", 0, false);
    testCodec(out, "h264", "mp4", 2, false);
    testCodec(out, "hevc", "mp4", 0, false);
    testCodec(out, "mjpeg", "avi", 0, true);

    testAudio(out);

    // what stops it
    {
        const EncodeSettings src = baseSettings(pathJoin(out, "stop_src.mp4"), "h264", 0);
        check(makeSource(src).empty(), "the source for the refusals is made");
        auto render = [&](Project& p, RenderSpec& spec, const char* name) {
            FxRuntime fx;
            spec.smartRender = true;
            spec.enc.path = pathJoin(out, std::string("stop_") + name + ".mp4");
            std::string e, d;
            const bool ok = renderProjectAs(p, fx, spec, 0, 2.0, nullptr, &e, MediaPool::Opener(), &d);
            return ok ? d : "failed: " + e;
        };
        {
            Project p = projectOf(src.path, 0.2, 2.0);
            p.tracks[0].events[0].fadeIn = 0.5;
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            const std::string d = render(p, spec, "fade");
            check(copiedOf(d) == 0 && d.find("nothing copied") != std::string::npos, "a fade copies nothing", d);
        }
        {
            Project p = projectOf(src.path, 0.2, 2.0);
            p.tracks[0].events[0].rate = 2.0;
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            const std::string d = render(p, spec, "rate");
            check(copiedOf(d) == 0, "a changed speed copies nothing", d);
        }
        {
            Project p = projectOf(src.path, 0.2, 2.0);
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            spec.enc.videoFamily = "hevc";
            const std::string d = render(p, spec, "codec");
            check(copiedOf(d) == 0 && d.find("nothing copied") != std::string::npos, "another codec copies nothing", d);
        }
        {
            Project p = projectOf(src.path, 0.2, 2.0);
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            spec.width = 64;
            spec.height = 36;
            const std::string d = render(p, spec, "size");
            check(copiedOf(d) == 0, "another size copies nothing", d);
        }
        {
            Project p = projectOf(src.path, 0.2, 2.0);
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            spec.enc.profile = "baseline";
            const std::string d = render(p, spec, "profile");
            check(copiedOf(d) == 0, "another profile copies nothing", d);
        }
        {
            Project p = projectOf(src.path, 0.2, 2.0);
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            spec.twoPass = true;
            const std::string d = render(p, spec, "twopass");
            check(copiedOf(d) == 0, "two passes copy nothing", d);
        }
        {
            // a container that wants the parameter sets in the stream (MPEG-TS): encoded in full
            Project p = projectOf(src.path, 0.2, 2.0);
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            FxRuntime fx;
            spec.smartRender = true;
            spec.enc.path = pathJoin(out, "stop_ts.ts");
            std::string e, d;
            const bool ok = renderProjectAs(p, fx, spec, 0, 2.0, nullptr, &e, MediaPool::Opener(), &d);
            check(ok && copiedOf(d) == 0, "a container without a global header copies nothing", d + e);
        }
        {
            // an event that does not sit on the file's frames
            Project p = projectOf(src.path, 0.213, 2.0);
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            const std::string d = render(p, spec, "offgrid");
            check(copiedOf(d) == 0, "an event off the file's frames copies nothing", d);
        }
        {
            // a second event on top: the lower one is covered
            Project p = projectOf(src.path, 0.2, 2.0);
            Track t2 = p.tracks[0];
            t2.id = p.newId();
            t2.events[0].id = p.newId();
            p.tracks.push_back(t2);
            RenderSpec spec;
            spec.enc = baseSettings("", "h264", 0);
            const std::string d = render(p, spec, "twoevents");
            check(copiedOf(d) == 0, "two events on the same frames copy nothing", d);
        }
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
