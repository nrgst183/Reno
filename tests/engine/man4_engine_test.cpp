// Engine tests for features from the VEGAS Pro 18 manual pages 365-522 and VEGAS Pro 23 pages 919-1178:
//   - closed captions (markup, channel display, caption file formats, CEA-608 characters, SCC timing);
//   - ACES color management without OpenColorIO (camera curves, view transforms, 32-bit float projects);
//   - Render As: 360° metadata, 5.1 templates, multichannel mapping, multiple mono files, the loudness log;
//   - the loudness meter's 5.1 weighting, per-channel true peaks and Reset Clip;
//   - Beat / Tempo Detection, Auto Ducking, automation modes, thinning and flipping envelope points.
//
//   man4_engine_test <output folder>
#include "../../src/engine/engine.h"
#include "../../src/audiofx/audioplugin.h"
#include "../../src/engine/automation.h"
#include "../../src/engine/audioanalysis.h"
#include "../../src/engine/captions.h"
#include "../../src/engine/colormgmt.h"
#include "../../src/engine/loudness.h"
#include "../../src/engine/renderas.h"
#include "../../src/media/ffmpeg_common.h"
#include <cmath>
#include <cstdio>
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
static std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
static const double kPi = 3.14159265358979323846;
static const int kRate = 48000;

// ===================================================================================================== captions
static void testCaptions(const std::string& out)
{
    const std::string pop = popOnMarkup("First Row[BR]Second Row");
    check(pop == "{RCL}{ENM}{R14In00Wh}{TabOff1}First Row{R15In00Wh}{TabOff1}Second Row{EDM}{EOC}", "plain text becomes VEGAS's pop-on markup", pop);
    check(captionText(pop) == "First Row[BR]Second Row" && captionText("{RU2}{CR}{R15In00Wh}{TabOff1}Live") == "Live", "the text of a caption's markup");
    const std::string longText(40, 'x');
    check(captionText(popOnMarkup(longText + " end")).find("[BR]") != std::string::npos, "a row longer than 32 characters is broken");
    check(isCaptionCommand("608CC3") && isCaptionCommand("CEA608CC1") && !isCaptionCommand("TEXT") && !isCaptionCommand("608CC5"), "608CC1..4 (and CEA608CC1..4) are captions");

    // what the screen shows
    std::vector<Caption> caps = {{1.0, 1, pop}, {2.0, 1, "{EDM}"}, {3.0, 1, "{RDC}{R10In08Rd}Paint"}, {4.0, 1, "{RU2}{CR}{R15In00Wh}One"},
                                 {5.0, 1, "{RU2}{CR}{R15In00Wh}Two"}, {6.0, 1, "{RU2}{CR}{R15In00Wh}Three"}};
    auto rows = [&](double t) {
        std::string s;
        for (const CaptionRow& r : captionScreenAt(caps, t)) s += strfmt("%d:%d:%s:%s;", r.row, r.column, r.text.c_str(), r.color.c_str());
        return s;
    };
    check(rows(0.5) == "" && rows(1.5) == "14:1:First Row:Wh;15:1:Second Row:Wh;" && rows(2.5) == "", "pop-on: shown at its time, cleared by {EDM}", rows(1.5));
    check(rows(3.5) == "10:8:Paint:Rd;", "paint-on: shown at once, in its colour and indent", rows(3.5));
    check(rows(5.5) == "10:8:Paint:Rd;14:0:One:Wh;15:0:Two:Wh;" || rows(5.5).find("14:0:One:Wh;15:0:Two:Wh;") != std::string::npos, "roll-up: lines move up", rows(5.5));
    check(rows(6.5).find("One") == std::string::npos && rows(6.5).find("14:0:Two:Wh;15:0:Three:Wh;") != std::string::npos, "roll-up of two rows: the oldest line goes", rows(6.5));

    // SCC: special characters and timing
    std::vector<Caption> scc = {{1.0, 1, popOnMarkup("Caf\xC3\xA9 \xE2\x99\xAA ma\xC3\xB1" "ana")}, {3.0, 2, popOnMarkup("Channel two")}, {4.0, 1, "{EDM}"}};
    const std::string text = sccText(scc);
    std::vector<Caption> back;
    std::string err;
    const bool ok = parseScc(text, back, &err);
    check(ok && back.size() == 3 && captionText(back[0].markup) == "Caf\xC3\xA9 \xE2\x99\xAA ma\xC3\xB1" "ana" && std::fabs(back[0].time - 1.0) < 0.04 &&
              back[1].channel == 2 && captionText(back[1].markup) == "Channel two" && back[2].markup == "{EDM}",
          "SCC: written and read back (accents, the music note, CC2, the end of caption on its time)", back.empty() ? err : back[0].markup + " " + fmt(back[0].time));
    // a known SCC line (a pop-on caption as captioning tools write it)
    std::vector<Caption> known;
    parseScc("Scenarist_SCC V1.0\r\n\r\n00:00:00;00\t94ae 94ae 9420 9420 9470 9470 4865 6c6c ef80 942f 942f\r\n\r\n00:00:02;00\t942c 942c\r\n", known, &err);
    check(known.size() == 2 && captionText(known[0].markup) == "Hello" && known[1].markup == "{EDM}" && std::fabs(known[1].time - 2.0 * 1.001) < 0.01,
          "SCC: a caption from another program (drop-frame timecode)", known.empty() ? err : known[0].markup);

    // the other formats
    auto writeText = [&](const std::string& name, const std::string& body) {
        const std::string path = pathJoin(out, name);
        writeFile(path, body);
        return path;
    };
    std::vector<Caption> got;
    check(readCaptionFile(writeText("a.srt", "1\n00:00:01,000 --> 00:00:02,500\nHello <b>you</b>\n\n"), 30, got, &err) && got.size() == 2 &&
              captionText(got[0].markup) == "Hello you" && std::fabs(got[1].time - 2.5) < 1e-9, "SubRip (.srt)");
    got.clear();
    check(readCaptionFile(writeText("a.smi", "<SAMI><BODY><SYNC Start=1000><P Class=ENUSCC>One<br>two<SYNC Start=1500><P Class=FRFRCC>Un"
                                             "<SYNC Start=2000><P Class=ENUSCC>&nbsp;</BODY></SAMI>"), 30, got, &err) &&
              got.size() == 3 && captionText(got[0].markup) == "One[BR]two" && got[1].channel == 3 && got[2].markup == "{EDM}",
          "SAMI (.smi): line breaks, the second language on CC3, &nbsp; clears", strfmt("%d", (int)got.size()));
    got.clear();
    check(readCaptionFile(writeText("a.rt", "<window><time begin=\"00:00:01.00\"/><clear/>Real text<time begin=\"00:00:03.50\"/>More</window>"), 30, got, &err) &&
              got.size() == 2 && std::fabs(got[1].time - 3.5) < 1e-9 && captionText(got[1].markup) == "More", "RealText (.rt)");
    got.clear();
    check(readCaptionFile(writeText("a.txt", "{QTtext}{font:Arial}\n[00:00:01.00]\nQuick\n[00:00:02.00]\n\n"), 30, got, &err) && got.size() == 2 &&
              captionText(got[0].markup) == "Quick" && got[1].markup == "{EDM}", "QuickTime text (.txt)");
    got.clear();
    check(readCaptionFile(writeText("b.txt", "00:00:01:15 A transcript line\n00:00:03:00 Another\n"), 30, got, &err) && got.size() == 2 &&
              std::fabs(got[0].time - 1.5) < 1e-6, "a transcript (.txt) with frame timecodes");
    got.clear();
    check(readCaptionFile(writeText("a.sub", "0001\t00:00:01:00\t00:00:02:00\tFirst\nmore\n\n0002\t00:00:02:00\t00:00:03:00\tSecond\n"), 30, got, &err) &&
              got.size() == 3 && captionText(got[0].markup) == "First[BR]more" && got[2].markup == "{EDM}", "DVD Architect subtitles (.sub)");
    got.clear();
    // MacCaption: a CDP whose cc_data carries the pairs of "Hi" as a paint-on caption (field 1)
    const std::string cdp = "9669" "1E" "4F" "77" "0000" "72" "E5" "FC9429" "FC9452" "FCC8E9" "FA0000" "FA0000" "74" "0000" "BB";
    check(readCaptionFile(writeText("a.mcc", "File Format=MacCaption_MCC V1.0\n\nTime Code Rate=30DF\n\n00:00:01;00\t" + cdp + "\n"), 30, got, &err) &&
              got.size() == 1 && captionText(got[0].markup) == "Hi", "MacCaption (.mcc): the CEA-608 pairs in its packets", got.empty() ? err : got[0].markup);

    // writing
    std::vector<Caption> two = {{1.0, 1, popOnMarkup("One")}, {2.0, 1, popOnMarkup("Two[BR]lines")}};
    const std::string srt = pathJoin(out, "w.srt"), smi = pathJoin(out, "w.smi"), sub = pathJoin(out, "w.sub"), qt = pathJoin(out, "w.txt");
    bool written = writeCaptionFile(srt, CaptionFormat::Srt, two, 4.0, 30, &err) && writeCaptionFile(smi, CaptionFormat::Smi, two, 4.0, 30, &err) &&
                   writeCaptionFile(sub, CaptionFormat::Sub, two, 4.0, 30, &err) && writeCaptionFile(qt, CaptionFormat::QuickTime, two, 4.0, 30, &err);
    std::string s1;
    readFile(srt, s1);
    check(written && s1.find("00:00:02,000 --> 00:00:04,000\r\nTwo\r\nlines") != std::string::npos && fileExists(pathJoin(out, "w.asx")) && fileExists(pathJoin(out, "w.smil")),
          "the writers: SRT (each caption until the next, the last until the end), SAMI with its ASX, QuickTime with its SMIL");
    for (const std::string& f : {smi, sub, qt}) {
        std::vector<Caption> rb;
        readCaptionFile(f, 30, rb, &err);
        bool has = false;
        for (const Caption& c : rb) has = has || captionText(c.markup) == "Two[BR]lines";
        check(has, "a written " + f.substr(f.size() - 3) + " reads back");
    }
    // nonsense does not crash
    std::vector<Caption> junk;
    for (const char* name : {"j.scc", "j.srt", "j.smi", "j.rt", "j.txt", "j.mcc", "j.sub"}) readCaptionFile(writeText(name, "\x01\xff{{{{<<<SYNC Start=\n-->"), 30, junk, &err);
    junk.clear();
    parseScc("Scenarist_SCC V1.0\n\n00:00:00:00\t1111 1212 9420 ffff zzzz 94\n", junk, &err);
    check(true, "damaged caption files are read without trouble");
}

// ===================================================================================================== color
static void testColor()
{
    auto enc = [](const char* cs, float v) { float c[3] = {v, v, v}; colorFromSceneLinear(c, cs); return c[0]; };
    auto dec = [](const char* cs, float v) { float c[3] = {v, v, v}; colorToSceneLinear(c, cs); return c[0]; };
    check(std::fabs(enc("slog3", 0.18f) * 1023 - 420) < 1.0, "S-Log3: 18% grey is code 420", fmt(enc("slog3", 0.18f) * 1023));
    check(std::fabs(enc("logc", 0.18f) - 0.391) < 0.005, "ARRI LogC (EI 800): 18% grey is 0.391", fmt(enc("logc", 0.18f)));
    check(std::fabs(enc("acescc", 0.18f) - 0.4135) < 0.002 && std::fabs(enc("acescct", 0.18f) - 0.4135) < 0.002, "ACEScc / ACEScct: 18% grey is 0.4135");
    check(std::fabs(enc("vlog", 0.18f) - 0.423) < 0.005, "Panasonic V-Log: 18% grey is 0.423", fmt(enc("vlog", 0.18f)));
    bool round = true;
    std::string worst;
    for (const char* cs : {"rec709", "srgb", "rec2020", "acescc", "acescct", "acesproxy", "adx10", "adx16", "slog1", "slog2_3200", "slog2_5500", "slog3", "logc", "clog",
                           "protune", "vlog", "log3g10", "log", "aces", "acescg"})
        for (float v : {0.02f, 0.18f, 0.5f, 0.9f}) {
            float c[3] = {v, v * 0.5f, v * 0.25f};
            colorFromSceneLinear(c, cs);
            colorToSceneLinear(c, cs);
            if (std::fabs(c[0] - v) > 2e-3 * std::max(1.0f, v) || std::fabs(c[1] - v * 0.5f) > 2e-3 || std::fabs(c[2] - v * 0.25f) > 2e-3) {
                round = false;
                worst = std::string(cs) + " " + fmt(v) + " -> " + fmt(c[0]);
            }
        }
    check(round, "every color space's curve and its inverse give the colour back", worst);
    // the view transforms: black stays black, they rise with the light, highlights roll off below 1
    bool views = true;
    for (const auto& v : viewTransforms("1.0")) {
        if (v.id == "off") continue;
        const float a = enc(v.id.c_str(), 0.0f), b = enc(v.id.c_str(), 0.18f), c = enc(v.id.c_str(), 1.0f), d = enc(v.id.c_str(), 16.0f);
        if (!(a <= 0.12f && b > a && c > b && d >= c && d <= 1.001f)) { views = false; worst = v.id; }
    }
    check(views, "every view transform: black dark, rising, highlights rolled off within the display", worst);
    check(enc("rrt_srgb", 0.18f) < enc("srgb", 0.18f), "the ACES RRT is darker than plain sRGB at mid grey (as the manual says)", fmt(enc("rrt_srgb", 0.18f)));
    check(colorSpaceLabel("slog3") == "Input - Sony - S-Log3 - S-Gamut3" && mediaColorSpaces("0.7").size() == 9 && viewTransforms("0.7").size() == 6 &&
              viewTransforms("1.0").size() == 23 && renderColorSpaces().front().label == "Default",
          "VEGAS's lists (ACES 0.7's and 1.0's)");
    (void)dec;
}

// ===================================================================================================== media
// A clip: 'seconds' long, 64x36, a flat grey picture of 'level', and a tone of 'freq' Hz at 0.3 (0: no sound).
static std::string writeClip(const std::string& path, double seconds, int level, double freq)
{
    EncodeSettings es;
    es.path = path;
    es.video = level >= 0;
    es.audio = freq > 0;
    es.width = 64;
    es.height = 36;
    es.fpsNum = 30;
    es.fpsDen = 1;
    es.sampleRate = kRate;
    es.videoFamily = "rawvideo";
    es.audioFamily = "pcm";
    std::string err;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
    if (!w) return "cannot write " + path + ": " + err;
    const int frames = (int)std::lround(seconds * 30);
    ImagePtr img = Image::make(64, 36, PixelDepth::Byte, true);
    for (int y = 0; y < 36; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t* p = img->row(y) + x * 4;
            p[0] = p[1] = p[2] = (uint8_t)std::max(0, level);
            p[3] = 255;
        }
    std::vector<float> l(1600), r(1600);
    for (int f = 0; f < frames; ++f) {
        if (es.video) w->writeVideo(*img);
        if (es.audio) {
            for (int k = 0; k < 1600; ++k) l[(size_t)k] = r[(size_t)k] = (float)(0.3 * std::sin(2 * kPi * freq * (f * 1600 + k) / kRate));
            w->writeAudio(l.data(), r.data(), 1600);
        }
    }
    if (!w->finish(&err)) return "cannot finish " + path + ": " + err;
    return std::string();
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

static Event eventOf(uint64_t id, uint64_t media, double len)
{
    Event e;
    e.id = id;
    e.mediaId = media;
    e.start = 0;
    e.length = len;
    return e;
}

static double tone(const float* s, int n, double freq)
{
    double c = 0, d = 0;
    for (int k = 0; k < n; ++k) {
        c += s[k] * std::cos(2 * kPi * freq * k / kRate);
        d += s[k] * std::sin(2 * kPi * freq * k / kRate);
    }
    return 2.0 * std::sqrt(c * c + d * d) / std::max(1, n);
}

static void testPictures(const std::string& out)
{
    const std::string clip = pathJoin(out, "grey.avi");
    const std::string err = writeClip(clip, 1.0, 128, 0);
    check(err.empty(), "a test clip is written", err);
    Project p;
    p.settings.width = 64;
    p.settings.height = 36;
    MediaItem m = mediaOf(p, clip);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    t.events.push_back(eventOf(p.newId(), m.id, 1.0));
    p.tracks.push_back(t);
    MediaPool pool;
    FxRuntime fx;
    VideoEngine eng(pool, fx);
    FrameOptions fo;
    fo.interactive = false;
    auto grey = [&](const Project& pr, const FrameOptions& o) {
        ImagePtr img = eng.render(pr, 0.5, o);
        return img ? (int)img->row(18)[32 * 4 + 1] : -1;
    };
    const int eight = grey(p, fo);
    Project full = p;
    full.settings.pixelFormat = "32bit";       // 32-bit floating point (full range), compositing gamma 1.000: linear light
    full.settings.pixelRange = "full";
    full.settings.compositingGamma = 1.0;
    const int off = grey(full, fo);
    full.settings.viewTransform = "rrt_srgb";
    const int aces = grey(full, fo);
    Project slog = full;
    slog.media[0].colorSpace = "slog3";
    const int slogGrey = grey(slog, fo);
    FrameOptions linear = fo;
    linear.colorSpace = "aces";
    const int acesOut = grey(full, linear);
    check(eight >= 126 && eight <= 130 && std::abs(off - eight) <= 1, "full range with the View transform Off: as the 8-bit project", strfmt("%d / %d", eight, off));
    check(aces >= 0 && aces < eight - 10, "through the ACES RRT (sRGB): darker and more contrasty", strfmt("%d", aces));
    check(slogGrey > aces, "a file marked S-Log3: its grey is brighter light (128 in S-Log3 is above 18%)", strfmt("%d", slogGrey));
    check(acesOut >= 0 && acesOut < aces, "a render template's ACES color space: the linear ACES values", strfmt("%d", acesOut));
    // the preview's way: interactive, at half size
    FrameOptions pv;
    pv.scale = 0.5;
    auto greyAt = [&](const Project& pr) {
        ImagePtr img = eng.render(pr, 0.5, pv);
        return img ? (int)img->row(img->h / 2)[(img->w / 2) * 4 + 1] : -1;
    };
    Project offPr = full;
    offPr.settings.viewTransform = "off";
    check(std::abs(greyAt(offPr) - eight) <= 1 && greyAt(full) < eight - 10, "the preview (interactive, half size) too", strfmt("%d / %d", greyAt(offPr), greyAt(full)));
}

// reads a file's audio with FFmpeg: channel count, and each channel's samples (up to 'max')
static int readChannels(const std::string& path, std::vector<std::vector<float>>& ch, int max = kRate)
{
    std::string err;
    std::unique_ptr<MediaReader> r = openMedia(path, &err);
    if (!r) return 0;
    const int n = r->info().channels;
    ch.assign((size_t)std::max(0, n), std::vector<float>((size_t)max));
    std::vector<float> l((size_t)max), rr((size_t)max);
    for (int c = 0; c < n; ++c) {
        r->audioPick(0, max, kRate, c, 1, l.data(), rr.data());
        ch[(size_t)c] = l;
    }
    return n;
}

static bool hasSpherical(const std::string& path)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return false;
    avformat_find_stream_info(fc, nullptr);
    bool found = false;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVCodecParameters* cp = fc->streams[i]->codecpar;
        if (cp->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        const AVPacketSideData* sd = av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_SPHERICAL);
        if (sd && sd->size >= sizeof(AVSphericalMapping) && ((const AVSphericalMapping*)sd->data)->projection == AV_SPHERICAL_EQUIRECTANGULAR) found = true;
    }
    avformat_close_input(&fc);
    return found;
}

static void testRender(const std::string& out)
{
    // ---- 360°: the spherical metadata
    const std::string clip = pathJoin(out, "tones.avi");
    std::string err = writeClip(clip, 1.0, 100, 500);
    const std::string tone2 = pathJoin(out, "tone2.wav");
    err += writeClip(tone2, 1.0, -1, 900);
    check(err.empty(), "test media written", err);
    Project p;
    p.settings.width = 64;
    p.settings.height = 36;
    MediaItem v = mediaOf(p, clip), a2 = mediaOf(p, tone2);
    {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.events.push_back(eventOf(p.newId(), v.id, 1.0));
        p.tracks.push_back(t);
    }
    FxRuntime fx;
    for (const char* ext : {".mp4", ".mkv"}) {
        RenderSpec spec;
        spec.enc.path = pathJoin(out, std::string("sphere") + ext);
        spec.enc.audio = false;
        spec.enc.videoFamily = "mpeg4";
        spec.spherical = true;
        std::string e;
        const bool ok = renderProjectAs(p, fx, spec, 0, 0.5, nullptr, &e);
        check(ok && hasSpherical(spec.enc.path), std::string("a 360\xC2\xB0 render is marked equirectangular (") + ext + ")", e);
    }
    {
        RenderSpec spec;
        spec.enc.path = pathJoin(out, "flat.mp4");
        spec.enc.audio = false;
        spec.enc.videoFamily = "mpeg4";
        std::string e;
        check(renderProjectAs(p, fx, spec, 0, 0.5, nullptr, &e) && !hasSpherical(spec.enc.path), "an ordinary render is not");
    }

    // ---- sound: a track on Bus A (500 Hz), a track on Bus B (900 Hz)
    Project s;
    MediaItem m1 = mediaOf(s, clip), m2 = mediaOf(s, tone2);
    Bus busA, busB;
    busA.id = s.newId();
    busA.name = "Bus A";
    busB.id = s.newId();
    busB.name = "Bus B";
    s.buses = {busA, busB};
    for (int k = 0; k < 2; ++k) {
        Track t;
        t.id = s.newId();
        t.type = TrackType::Audio;
        t.output = k == 0 ? busA.id : busB.id;
        t.events.push_back(eventOf(s.newId(), k == 0 ? m1.id : m2.id, 1.0));
        s.tracks.push_back(t);
    }
    {
        // the bus tap
        MediaPool pool;
        AudioEngine eng(pool, fx);
        std::map<uint64_t, double> heard;
        eng.busTap = [&](uint64_t id, const float* l, const float*, int n) { heard[id] = std::max(tone(l, n, id == busA.id ? 500 : 900), heard[id]); };
        std::vector<float> l(4800), r(4800);
        eng.render(s, 4800, 4800, l.data(), r.data(), true);
        check(heard.size() == 2 && heard[busA.id] > 0.1 && heard[busB.id] > 0.1, "the audio engine's bus tap hears each bus", fmt(heard[busA.id]) + " " + fmt(heard[busB.id]));
    }
    {
        RenderSpec spec;
        spec.enc.path = pathJoin(out, "mapped.wav");
        spec.enc.video = false;
        spec.enc.audioFamily = "pcm";
        RenderSpec::ChannelSource ca, cb, cm;
        ca.bus = (int64_t)busA.id;
        cb.bus = (int64_t)busB.id;
        cb.mono = true;
        spec.channelMap = {ca, cb, cm};       // Bus A (2), Bus B mono (1), the master (2)
        std::string e;
        const bool ok = renderProjectAs(s, fx, spec, 0, 1.0, nullptr, &e);
        std::vector<std::vector<float>> ch;
        const int n = ok ? readChannels(spec.enc.path, ch, 9600) : 0;
        check(ok && n == 5, "multichannel mapping: two stereo outputs and a mono downmix make a 5-channel file", strfmt("%d channels %s", n, e.c_str()));
        if (n == 5) {
            const int k0 = 2400, len = 4800;
            check(tone(&ch[0][k0], len, 500) > 0.15 && tone(&ch[0][k0], len, 900) < 0.01 && tone(&ch[1][k0], len, 500) > 0.15,
                  "channels 1-2: Bus A alone", fmt(tone(&ch[0][k0], len, 500)));
            check(tone(&ch[2][k0], len, 900) > 0.15 && tone(&ch[2][k0], len, 500) < 0.01, "channel 3: Bus B's mono downmix alone");
            check(tone(&ch[3][k0], len, 500) > 0.15 && tone(&ch[3][k0], len, 900) > 0.15, "channels 4-5: the master, both");
        }
    }
    {
        // multiple mono (a stereo project: Left and Right)
        RenderSpec spec;
        spec.enc.path = pathJoin(out, "mono.wav");
        spec.enc.video = false;
        spec.enc.audioFamily = "pcm";
        spec.multipleMono = true;
        std::string e;
        const bool ok = renderProjectAs(s, fx, spec, 0, 1.0, nullptr, &e);
        const std::vector<std::string> files = multipleMonoPaths(spec.enc.path, 2);
        std::vector<std::vector<float>> ch;
        check(ok && files.size() == 2 && pathFileName(files[1]) == "mono Right.wav" && readChannels(files[0], ch, 100) == 1 && readChannels(files[1], ch, 100) == 1 &&
                  !fileExists(spec.enc.path),
              "multiple mono: one mono file a channel, named as VEGAS names them", e);
        check(multipleMonoPaths("x/My Film.wav", 6)[4] == "x/My Film Left Surround.wav", "a 5.1 project's six files' names");
    }
    {
        // a 5.1 project rendered with a 5.1 template: six channels, the centre speaker's sound in the centre channel
        Project six = s;
        six.settings.audioChannels = 6;
        for (Track& t : six.tracks) {
            t.output = 0;
            t.surround.route = "center";
        }
        RenderSpec spec;
        spec.enc.path = pathJoin(out, "six.wav");
        spec.enc.video = false;
        spec.enc.audioFamily = "pcm";
        spec.enc.audioChannels = 6;
        spec.loudnessLog = pathJoin(out, "six_loud.txt");
        std::string e;
        const bool ok = renderProjectAs(six, fx, spec, 0, 1.0, nullptr, &e);
        std::vector<std::vector<float>> ch;
        const int n = ok ? readChannels(spec.enc.path, ch, 9600) : 0;
        check(n == 6 && tone(&ch[2][2400], 4800, 500) > 0.1 && tone(&ch[0][2400], 4800, 500) < 0.02,
              "a 5.1 render has the six channels (a centre-only track in the centre channel)", strfmt("%d channels", n));
        std::string log;
        readFile(spec.loudnessLog, log);
        check(log.find("Integrated:") != std::string::npos && log.find("EBU R 128") != std::string::npos && log.find("00:00:01.000") != std::string::npos &&
                  log.find("Time") != std::string::npos,
              "the loudness log beside it (mode, values, the table)");
    }
}

static void testLoudness()
{
    // the same signal in L and R, or in Ls and Rs: the surround channels weigh +1.5 dB
    const int n = kRate * 2;
    std::vector<float> sig((size_t)n), zero((size_t)n, 0.f);
    for (int k = 0; k < n; ++k) sig[(size_t)k] = (float)(0.25 * std::sin(2 * kPi * 1000 * k / kRate));
    LoudnessMeter front(kRate), rear(kRate), lfe(kRate);
    const float* f6[6] = {sig.data(), sig.data(), zero.data(), zero.data(), zero.data(), zero.data()};
    const float* r6[6] = {zero.data(), zero.data(), zero.data(), zero.data(), sig.data(), sig.data()};
    const float* l6[6] = {zero.data(), zero.data(), zero.data(), sig.data(), zero.data(), zero.data()};
    front.processChannels((const float* const*)f6, 6, n);
    rear.processChannels((const float* const*)r6, 6, n);
    lfe.processChannels((const float* const*)l6, 6, n);
    check(std::fabs(rear.integrated() - front.integrated() - 1.49) < 0.05, "5.1 loudness: the surround channels count +1.5 dB",
          fmt(front.integrated()) + " / " + fmt(rear.integrated()));
    check(lfe.integrated() <= LoudnessMeter::kSilence + 1e-6, "5.1 loudness: the LFE does not count");
    check(std::fabs(rear.channelTruePeak(4) - 20 * std::log10(0.25)) < 0.2 && rear.channelTruePeak(0) < -100, "true peaks per channel", fmt(rear.channelTruePeak(4)));
    rear.resetPeak();
    check(rear.truePeak() < -100 && rear.integrated() > -40, "Reset Clip clears the peaks, not the loudness");
    // the blocking filter: a DC offset counts in the plain peak, not in the filtered one... and the larger is taken
    LoudnessMeter dc(kRate);
    dc.setPeakBlocking(true);
    std::vector<float> off((size_t)n);
    for (int k = 0; k < n; ++k) off[(size_t)k] = 0.5f + (float)(0.1 * std::sin(2 * kPi * 1000 * k / kRate));
    dc.process(off.data(), off.data(), n);
    check(std::fabs(dc.truePeak() - 20 * std::log10(0.6)) < 0.3, "True Peak Blocking Filter: the larger of the filtered and unfiltered peaks", fmt(dc.truePeak()));
    std::vector<LoudnessLogPoint> pts(2);
    pts[0].time = 1;
    pts[1].time = 2;
    const std::string text = loudnessLogText("file.wav", "48000 Hz", true, 0, 2, front, pts);
    check(text.find("ATSC A/85") != std::string::npos && text.find("00:00:02.000") != std::string::npos, "the loudness log's text (ATSC mode, every second)");
}


// ===================================================================================================== VEGAS Pro 23
// A mono click track: a 40 ms burst of 80 Hz plus a noise tick on every beat of 'bpm'.
static std::vector<float> clickTrack(double bpm, double seconds, double offset = 0.0)
{
    std::vector<float> m((size_t)(seconds * kRate), 0.0f);
    const double period = 60.0 / bpm;
    unsigned seed = 12345;
    for (double t = offset; t < seconds - 0.1; t += period) {
        const int at = (int)(t * kRate), len = (int)(0.04 * kRate);
        for (int k = 0; k < len && at + k < (int)m.size(); ++k) {
            seed = seed * 1664525u + 1013904223u;
            const double env = std::exp(-k / (0.012 * kRate));
            m[(size_t)(at + k)] += (float)(0.8 * env * std::sin(2 * kPi * 80.0 * k / kRate) + 0.05 * env * ((seed >> 16) / 32768.0 - 1.0));
        }
    }
    return m;
}

static void testAnalysis()
{
    for (double bpm : {128.0, 90.0, 140.0}) {
        double conf = 0;
        const double got = detectTempo(clickTrack(bpm, 24.0), kRate, &conf);
        check(std::fabs(got - bpm) < 2.0 || std::fabs(got - bpm * 2) < 2.0 || std::fabs(got - bpm / 2) < 1.0, strfmt("Tempo Detection finds %.0f BPM", bpm), fmt(got) + " conf " + fmt(conf));
    }
    check(detectTempo(std::vector<float>((size_t)(5 * kRate), 0.0f), kRate) == 0.0, "Tempo Detection of silence gives 0");
    check(detectTempo(std::vector<float>(100, 0.1f), kRate) == 0.0, "Tempo Detection of a too-short sound gives 0");
    const std::vector<float> clicks = clickTrack(120.0, 12.0);     // 24 beats, one every 0.5 s
    const std::vector<double> loose = detectBeats(clicks, kRate, 0.2), tight = detectBeats(clicks, kRate, 0.9);
    check(loose.size() >= 20 && loose.size() <= 30, "Beat Detection marks the clicks of a 120 BPM track", strfmt("%d markers", (int)loose.size()));
    check(tight.size() <= loose.size() && !tight.empty(), "a higher sensitivity slider gives fewer markers", strfmt("%d <= %d", (int)tight.size(), (int)loose.size()));
    bool near = !loose.empty();
    for (double t : loose) near = near && std::fabs(t / 0.5 - std::round(t / 0.5)) < 0.12;
    check(near, "the markers sit on the beats (within 60 ms)");
    check(detectBeats(std::vector<float>((size_t)(3 * kRate), 0.0f), kRate, 0.5).empty(), "Beat Detection of silence marks nothing");
    check(detectBeats(std::vector<float>(), kRate, 0.5).empty(), "Beat Detection of nothing does not crash");
}

static void testAutomationData()
{
    // thinning: a straight run of points loses its middle
    Envelope e;
    e.kind = "pan";
    for (int k = 0; k <= 10; ++k) { EnvPoint pt; pt.time = k; pt.value = -1.0 + 0.2 * k; e.points.push_back(pt); }
    const int taken = thinEnvelope(e, -1e18, 1e18, thinTolerance("pan"));
    check(taken == 9 && e.points.size() == 2 && e.points.front().time == 0 && e.points.back().time == 10, "Thin All Points keeps the ends of a straight line", strfmt("%d taken", taken));
    Envelope bent;
    bent.kind = "pan";
    for (int k = 0; k <= 4; ++k) { EnvPoint pt; pt.time = k; pt.value = k == 2 ? 1.0 : 0.0; bent.points.push_back(pt); }
    check(thinEnvelope(bent, -1e18, 1e18, thinTolerance("pan")) == 0 && bent.points.size() == 5, "Thin keeps a corner");
    Envelopes envs;
    Envelope v;
    v.kind = "volume";
    v.points = {EnvPoint{0, 1.0, CurveLinear}, EnvPoint{10, 0.0, CurveLinear}};
    envs.push_back(v);
    check(std::fabs(automationValue(envs, "volume", 0, 5.0, 1.0) - 0.5) < 1e-6 && automationValue(envs, "pan", 0, 5.0, 0.25) == 0.25, "a control shows the envelope's value, else its own setting");

    // writing at a time: the point near takes the value, else one is added
    Envelopes w;
    setEnvelopeAt(w, "volume", 0, 2.0, 0.5, 0.05, CurveLinear);
    setEnvelopeAt(w, "volume", 0, 2.02, 0.7, 0.05, CurveLinear);
    setEnvelopeAt(w, "volume", 0, 4.0, 0.9, 0.05, CurveLinear);
    check(w.size() == 1 && w[0].points.size() == 2 && std::fabs(w[0].points[0].value - 0.7) < 1e-9, "a control in automation mode edits the point at the cursor or adds one");

    // Touch and Latch takes
    Envelopes base;
    Envelope bv;
    bv.kind = "volume";
    bv.points = {EnvPoint{0, 1.0, CurveLinear}, EnvPoint{20, 1.0, CurveLinear}};
    base.push_back(bv);
    for (int latch = 0; latch < 2; ++latch) {
        AutomationTake take;
        take.kind = "volume";
        take.latch = latch != 0;
        take.hadEnvelope = true;
        take.base = bv.points;
        for (double t = 5.0; t <= 8.0; t += 0.25) take.write(t, 0.25, 0.1);
        take.latchTo = 12.0;
        Envelopes dst = base;
        take.apply(dst);
        const Envelope* r = &dst[0];
        const double at6 = r->valueAt(6.0, 1.0), at10 = r->valueAt(10.0, 1.0), at15 = r->valueAt(15.0, 1.0), at19 = r->valueAt(19.0, 1.0);
        if (!latch) check(std::fabs(at6 - 0.25) < 1e-6 && at10 > 0.99 && at19 > 0.99, "Automation Write (Touch): written while the control moved, the envelope back after", strfmt("%s %s %s", fmt(at6).c_str(), fmt(at10).c_str(), fmt(at19).c_str()));
        else check(std::fabs(at6 - 0.25) < 1e-6 && std::fabs(at10 - 0.25) < 1e-6 && at15 > 0.25 && at15 < at19, "Automation Write (Latch): the last value goes on until playback stops, then the envelope comes back", strfmt("%s %s %s %s", fmt(at6).c_str(), fmt(at10).c_str(), fmt(at15).c_str(), fmt(at19).c_str()));
    }
}

static void testDucking(const std::string& out)
{
    check(autoDuckGain(-60.0, -12.0, 0.0) == 1.0, "Auto Ducking: no change while the controller is quiet");
    const double g1 = autoDuckGain(-20.0, -12.0, 0.0), g2 = autoDuckGain(-6.0, -12.0, 1.0), g3 = autoDuckGain(-6.0, -12.0, 0.0);
    check(std::fabs(20.0 * std::log10(g1) + 12.0) < 0.5 && g2 < g3, "Auto Ducking: Floor sets the reduction, Peak Reduction lowers it for louder sound", fmt(20.0 * std::log10(g1)) + " dB, peak " + fmt(g2) + " vs " + fmt(g3));
    // the levels of a 1 s step: silence, speech for 3 s, silence
    std::vector<float> levels(100, -80.0f);
    for (int k = 30; k < 60; ++k) levels[(size_t)k] = -20.0f;
    const std::vector<EnvPoint> pts = duckPoints(levels, 0.1, 0.0, -12.0, 0.0);
    Envelope env;
    env.kind = "duck";
    env.points = pts;
    check(!pts.empty() && std::fabs(env.valueAt(0.0, 1.0) - 1.0) < 1e-6 && env.valueAt(4.5, 1.0) < 0.35 && env.valueAt(9.9, 1.0) > 0.99,
          "duckPoints: gain 1 before and after the speech, ducked during it", strfmt("%d points, %s during", (int)pts.size(), fmt(env.valueAt(4.5, 1.0)).c_str()));
    check(env.valueAt(3.05, 1.0) < 0.8 && env.valueAt(3.5, 1.0) < 0.5, "the gain comes down as soon as the sound starts", fmt(env.valueAt(3.05, 1.0)) + " " + fmt(env.valueAt(3.5, 1.0)));
    check(env.valueAt(6.2, 1.0) < 0.9, "and lets go slowly after it (a short hold)", fmt(env.valueAt(6.2, 1.0)));
    check(duckPoints(std::vector<float>(), 0.1, 0.0, -12.0, 0.0).size() <= 2, "no levels: no ducking points");

    // the audio engine: a listener track's music is ducked by the controller track's envelope
    const std::string music = pathJoin(out, "music.wav");
    std::string err = writeClip(music, 2.0, -1, 700);
    check(err.empty(), "ducking test media written", err);
    Project p;
    MediaItem m = mediaOf(p, music);
    Track ctrl, listener;
    ctrl.id = p.newId();
    ctrl.type = TrackType::Audio;
    ctrl.duckController = true;
    Envelope duck;
    duck.kind = "duck";
    duck.points = {EnvPoint{0, 1.0, CurveLinear}, EnvPoint{0.5, 1.0, CurveLinear}, EnvPoint{0.52, 0.1, CurveLinear}, EnvPoint{1.5, 0.1, CurveLinear}, EnvPoint{1.52, 1.0, CurveLinear}};
    ctrl.envelopes.push_back(duck);
    listener.id = p.newId();
    listener.type = TrackType::Audio;
    listener.duckListener = true;
    listener.events.push_back(eventOf(p.newId(), m.id, 2.0));
    p.tracks = {ctrl, listener};
    FxRuntime fx;
    MediaPool pool;
    auto level = [&](Project& pr, double t0) {
        AudioEngine eng(pool, fx);
        std::vector<float> l(4800), r(4800);
        eng.render(pr, (int64_t)(t0 * kRate), 4800, l.data(), r.data(), true);
        return tone(l.data(), 4800, 700);
    };
    const double before = level(p, 0.1), during = level(p, 0.8);
    check(before > 0.1 && during < before * 0.2 && during > before * 0.05, "a listener track is ducked while the controller's envelope is down", fmt(before) + " -> " + fmt(during));
    p.tracks[1].duckListener = false;
    check(std::fabs(level(p, 0.8) - before) < before * 0.1, "a track that is not a listener is not ducked");
}

// The Modern Equalizer stand-in: bands set by (parameter, natural value) and a tone sent through.
static double eqGain(const std::vector<std::pair<int, double>>& set, double freq, int channel = 0, double leftScale = 1.0, double rightScale = 1.0)
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.moderneq";
    std::string err;
    std::unique_ptr<AudioPlugin> pl = builtinAudioCreate(d, &err);
    if (!pl) return -1;
    pl->activate(kRate, 4800);
    const double mins[7] = {0, 0, 20, -24, 0.1, 0, 0}, maxs[7] = {1, 5, 20000, 24, 18, 4, 5};
    for (const auto& kv : set) {
        double mn, mx;
        if (kv.first >= 56) { const double gm[4][2] = {{-100, 200}, {0, 500}, {0, 200}, {-24, 24}}; mn = gm[kv.first - 56][0]; mx = gm[kv.first - 56][1]; }
        else { mn = mins[kv.first % 7]; mx = maxs[kv.first % 7]; }
        pl->paramSet(kv.first, (float)((kv.second - mn) / (mx - mn)));
    }
    std::vector<float> l(kRate), r(kRate);
    for (int k = 0; k < kRate; ++k) { l[(size_t)k] = (float)(0.25 * leftScale * std::sin(2 * kPi * freq * k / kRate)); r[(size_t)k] = (float)(0.25 * rightScale * std::sin(2 * kPi * freq * k / kRate)); }
    for (int off = 0; off < kRate; off += 4800) pl->process(&l[(size_t)off], &r[(size_t)off], 4800, off, true);
    const std::vector<float>& o = channel == 0 ? l : r;
    return tone(&o[kRate / 2], kRate / 4, freq) / 0.25;
}

static void testModernEq()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.moderneq";
    std::unique_ptr<AudioPlugin> pl = builtinAudioCreate(d, nullptr);
    check(pl && pl->paramCount() == 60 && pl->programCount() >= 6, "the Modern Equalizer has 8 bands of 7 settings, 4 global ones and presets", pl ? strfmt("%d parameters, %d presets", pl->paramCount(), pl->programCount()) : "none");
    check(std::fabs(eqGain({}, 1000) - 1.0) < 0.02, "with no band on it passes the sound unchanged", fmt(eqGain({}, 1000)));
    const double peak = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, 12}, {4, 1.0}}, 1000);
    check(std::fabs(20 * std::log10(peak) - 12.0) < 0.6, "a peaking band at 1 kHz with +12 dB gives +12 dB there", fmt(20 * std::log10(peak)) + " dB");
    check(std::fabs(20 * std::log10(eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, 12}, {4, 1.0}}, 100))) < 0.6, "and nothing far from it (100 Hz)");
    const double hp = eqGain({{0, 1}, {1, 0}, {2, 1000}}, 100), hp24 = eqGain({{0, 1}, {1, 0}, {2, 1000}, {6, 3}}, 100);
    check(hp < 0.05 && hp24 < hp && eqGain({{0, 1}, {1, 0}, {2, 1000}}, 8000) > 0.95, "a high pass cuts below its frequency (steeper with the slope) and keeps above", fmt(hp) + " " + fmt(hp24));
    check(eqGain({{0, 1}, {1, 1}, {2, 1000}}, 8000) < 0.05 && eqGain({{0, 1}, {1, 1}, {2, 1000}}, 100) > 0.95, "a low pass does the opposite");
    const double ls = eqGain({{0, 1}, {1, 2}, {2, 500}, {3, 10}}, 60), hs = eqGain({{0, 1}, {1, 3}, {2, 4000}, {3, -10}}, 12000);
    check(std::fabs(20 * std::log10(ls) - 10.0) < 1.0 && std::fabs(20 * std::log10(hs) + 10.0) < 1.0, "low and high shelves reach their gain", fmt(20 * std::log10(ls)) + " " + fmt(20 * std::log10(hs)));
    check(eqGain({{0, 1}, {1, 4}, {2, 1000}, {4, 4.0}}, 1000) > 0.9 && eqGain({{0, 1}, {1, 4}, {2, 1000}, {4, 4.0}}, 100) < 0.2, "a band pass keeps the band");
    // channel routing
    const double left = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, -12}, {4, 1.0}, {5, 1}}, 1000, 0), right = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, -12}, {4, 1.0}, {5, 1}}, 1000, 1);
    check(left < 0.3 && right > 0.97, "routed to Left, a band leaves the right channel alone", fmt(left) + " " + fmt(right));
    // Mid: a centred tone (the same on both) is cut; a tone in antiphase (all side) is not
    const double mid = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, -12}, {4, 1.0}, {5, 3}}, 1000, 0, 1.0, 1.0);
    const double midSide = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, -12}, {4, 1.0}, {5, 3}}, 1000, 0, 1.0, -1.0);
    check(mid < 0.3 && midSide > 0.97, "routed to Mid, a band acts on the centre and not on the sides", fmt(mid) + " " + fmt(midSide));
    const double side = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, -12}, {4, 1.0}, {5, 4}}, 1000, 0, 1.0, 1.0);
    const double sideSide = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, -12}, {4, 1.0}, {5, 4}}, 1000, 0, 1.0, -1.0);
    check(side > 0.97 && sideSide < 0.3, "routed to Side, the opposite", fmt(side) + " " + fmt(sideSide));
    // global controls
    check(std::fabs(eqGain({{59, 6}}, 1000) - std::pow(10.0, 6.0 / 20.0)) < 0.03, "Output Gain +6 dB");
    check(std::fabs(eqGain({{58, 0}}, 1000, 0, 1.0, -1.0)) < 0.02, "Stereo Width 0 makes the sound mono (an antiphase tone is gone)");
    check(eqGain({{57, 300}}, 100, 0, 1.0, -1.0) < 0.2 && eqGain({{57, 300}}, 1000, 0, 1.0, -1.0) > 0.9, "Mono Below removes the stereo from the lows only");
    const double scaled = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, 12}, {4, 1.0}, {56, 0}}, 1000), flipped = eqGain({{0, 1}, {1, 5}, {2, 1000}, {3, 12}, {4, 1.0}, {56, -100}}, 1000);
    check(std::fabs(scaled - 1.0) < 0.02 && std::fabs(20 * std::log10(flipped) + 12.0) < 0.8, "Global Scale 0% flattens the bands and -100% inverts them", fmt(scaled) + " " + fmt(20 * std::log10(flipped)));
    // state survives
    if (pl) {
        pl->paramSet(2, 0.5f);
        const std::string st = pl->saveState();
        std::unique_ptr<AudioPlugin> b = builtinAudioCreate(d, nullptr);
        check(b && b->loadState(st) && std::fabs(b->paramGet(2) - 0.5f) < 1e-3, "its settings are saved and loaded");
    }
}

static std::string probeCodecs(const std::string& path)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return "unreadable";
    avformat_find_stream_info(fc, nullptr);
    std::string r;
    for (unsigned i = 0; i < fc->nb_streams; ++i) r += std::string(avcodec_get_name(fc->streams[i]->codecpar->codec_id)) + " ";
    avformat_close_input(&fc);
    return r;
}

// Render As > MainConcept MPEG-2 (DVD streams): the program stream holds MPEG-2 video, and AC-3 sound when asked
static void testMpegRender(const std::string& out)
{
    const std::string clip = pathJoin(out, "tones.avi");
    const std::string tone2 = pathJoin(out, "tone2.wav");
    Project p;
    p.settings.width = 128;
    p.settings.height = 64;
    MediaItem v = mediaOf(p, clip), a2 = mediaOf(p, tone2);
    Track vt, at;
    vt.id = p.newId();
    vt.type = TrackType::Video;
    vt.events.push_back(eventOf(p.newId(), v.id, 1.0));
    at.id = p.newId();
    at.type = TrackType::Audio;
    at.events.push_back(eventOf(p.newId(), a2.id, 1.0));
    p.tracks = {vt, at};
    FxRuntime fx;
    for (int withAudio = 0; withAudio < 2; ++withAudio) {
        RenderSpec spec;
        spec.enc.path = pathJoin(out, withAudio ? "dvd.mpg" : "dvd_video.mpg");
        spec.enc.videoFamily = "mpeg2";
        spec.enc.audio = withAudio != 0;
        spec.enc.audioFamily = "ac3";
        spec.enc.audioKbps = 192;
        spec.enc.videoKbps = 4000;
        std::string e;
        const bool ok = renderProjectAs(p, fx, spec, 0, 1.0, nullptr, &e);
        const std::string codecs = ok ? probeCodecs(spec.enc.path) : e;
        const bool good = ok && codecs.find("mpeg2video") != std::string::npos && (withAudio ? codecs.find("ac3") != std::string::npos : codecs.find("ac3") == std::string::npos);
        check(good, withAudio ? "a .mpg render has MPEG-2 video and AC-3 sound" : "a Video Stream .mpg render has MPEG-2 video only", codecs);
    }
}

// Render As > MPEG-1 (Video CD and plain MPEG-1): MPEG-1 video and Layer II sound in an MPEG-1 system stream (pack
// headers 0x21..., not MPEG-2's 0x44...)
static void testMpeg1Render(const std::string& out)
{
    const std::string clip = pathJoin(out, "tones.avi");
    const std::string tone2 = pathJoin(out, "tone2.wav");
    Project p;
    p.settings.width = 352;
    p.settings.height = 288;
    MediaItem v = mediaOf(p, clip), a2 = mediaOf(p, tone2);
    Track vt, at;
    vt.id = p.newId();
    vt.type = TrackType::Video;
    vt.events.push_back(eventOf(p.newId(), v.id, 1.0));
    at.id = p.newId();
    at.type = TrackType::Audio;
    at.events.push_back(eventOf(p.newId(), a2.id, 1.0));
    p.tracks = {vt, at};
    FxRuntime fx;
    RenderSpec spec;
    spec.enc.path = pathJoin(out, "vcd.mpg");
    spec.enc.videoFamily = "mpeg1";
    spec.enc.audioFamily = "mp2";
    spec.enc.audioKbps = 224;
    spec.enc.outSampleRate = 44100;
    spec.enc.videoKbps = 1150;
    spec.enc.maxKbps = 1150;
    spec.enc.rateControl = "cbr";
    spec.enc.fpsNum = 25;
    spec.enc.fpsDen = 1;
    spec.enc.width = 352;
    spec.enc.height = 288;
    std::string e;
    const bool ok = renderProjectAs(p, fx, spec, 0, 1.0, nullptr, &e);
    const std::string codecs = ok ? probeCodecs(spec.enc.path) : e;
    check(ok && codecs.find("mpeg1video") != std::string::npos && codecs.find("mp2") != std::string::npos, "an MPEG-1 render has MPEG-1 video and Layer II sound", codecs);
    unsigned char head[6] = {0};
    if (FILE* f = std::fopen(spec.enc.path.c_str(), "rb")) {
        if (std::fread(head, 1, 6, f) != 6) head[0] = 1;
        std::fclose(f);
    }
    check(head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xBA && (head[4] & 0xF0) == 0x20, "it is an MPEG-1 system stream (pack header 0x21)");
}

int main(int argc, char** argv)
{
    const std::string out = argc > 1 ? argv[1] : "man4out";
    std::system(("mkdir -p '" + out + "'").c_str());
    testCaptions(out);
    testColor();
    testPictures(out);
    testRender(out);
    testLoudness();
    testAnalysis();
    testAutomationData();
    testDucking(out);
    testModernEq();
    testMpegRender(out);
    testMpeg1Render(out);
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
