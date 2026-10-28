// Tests Reno's own HEIC / HEIF / AVIF reader (src/media/heif.cpp): HEIF files are built here (the container by hand, the
// coded pictures with FFmpeg's HEVC and AV1 encoders, whichever this machine has) and read back through openMedia().
//   heif_test <out dir>
// Checks: one picture, a grid of tiles cropped to its output size, rotation and mirror, an alpha picture, and that a file
// of another kind is left alone. A codec this machine cannot encode is skipped (SKIP, not a failure).
#include "../../src/media/media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace reno;

namespace {

int gPassed = 0, gFailed = 0, gSkipped = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? gPassed : gFailed)++;
}

struct Coded {
    std::string config;     // hvcC / av1C contents
    std::string data;       // the coded picture as an item holds it
};

// One solid or quadrant picture, coded by 'encoder' (a 4:2:0 frame; gray levels or one colour).
// quadrant: the top left quarter is bright and the rest dark (gray only).
bool encode(const char* encoder, AVCodecID id, int w, int h, int y, int u, int v, bool quadrant, Coded& out)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder);
    if (!codec) return false;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->time_base = AVRational{1, 25};
    c->framerate = AVRational{25, 1};
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->color_range = AVCOL_RANGE_JPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    c->gop_size = 1;
    AVDictionary* o = nullptr;
    if (id == AV_CODEC_ID_HEVC) {
        av_dict_set(&o, "x265-params", "log-level=none:keyint=1:crf=4", 0);
        av_dict_set(&o, "preset", "ultrafast", 0);
    } else {
        av_dict_set(&o, "cpu-used", "8", 0);
        av_dict_set(&o, "crf", "4", 0);
        c->bit_rate = 0;
    }
    if (avcodec_open2(c, codec, &o) < 0) {
        av_dict_free(&o);
        avcodec_free_context(&c);
        return false;
    }
    av_dict_free(&o);
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    f->color_range = AVCOL_RANGE_JPEG;
    f->colorspace = AVCOL_SPC_BT709;
    av_frame_get_buffer(f, 0);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) f->data[0][j * f->linesize[0] + i] = (uint8_t)(quadrant ? (i < w / 2 && j < h / 2 ? 235 : 20) : y);
    for (int j = 0; j < h / 2; ++j)
        for (int i = 0; i < w / 2; ++i) {
            f->data[1][j * f->linesize[1] + i] = (uint8_t)u;
            f->data[2][j * f->linesize[2] + i] = (uint8_t)v;
        }
    f->pts = 0;
    // into an MP4 and back: the muxer turns the encoder's parameter sets into hvcC / av1C and the packet into the length-prefixed form
    const std::string tmp = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/heif_test_tile.mp4";
    AVFormatContext* oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "mp4", tmp.c_str());
    AVStream* st = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(st->codecpar, c);
    st->time_base = c->time_base;
    bool ok = avio_open(&oc->pb, tmp.c_str(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(oc, nullptr) >= 0;
    AVPacket* p = av_packet_alloc();
    avcodec_send_frame(c, f);
    avcodec_send_frame(c, nullptr);
    while (ok && avcodec_receive_packet(c, p) >= 0) {
        p->stream_index = 0;
        av_packet_rescale_ts(p, c->time_base, st->time_base);
        av_interleaved_write_frame(oc, p);
        av_packet_unref(p);
    }
    if (ok) av_write_trailer(oc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    av_frame_free(&f);
    avcodec_free_context(&c);
    if (!ok) {
        av_packet_free(&p);
        return false;
    }
    AVFormatContext* ic = nullptr;
    ok = avformat_open_input(&ic, tmp.c_str(), nullptr, nullptr) >= 0 && ic->nb_streams > 0;
    if (ok) {
        const AVCodecParameters* cp = ic->streams[0]->codecpar;
        out.config.assign((const char*)cp->extradata, (size_t)cp->extradata_size);
        ok = cp->extradata_size > 0 && av_read_frame(ic, p) >= 0;
        if (ok) out.data.assign((const char*)p->data, (size_t)p->size);
    }
    if (ic) avformat_close_input(&ic);
    av_packet_free(&p);
    std::remove(tmp.c_str());
    return ok;
}

// ---- the container
void put16(std::string& s, unsigned v) { s += (char)(v >> 8); s += (char)v; }
void put32(std::string& s, unsigned v) { put16(s, v >> 16); put16(s, v & 0xffff); }
std::string box(const char* type, const std::string& body)
{
    std::string s;
    put32(s, (unsigned)body.size() + 8);
    s += type;
    return s + body;
}
std::string fullbox(const char* type, int version, unsigned flags, const std::string& body)
{
    std::string b;
    b += (char)version;
    b += (char)(flags >> 16);
    b += (char)(flags >> 8);
    b += (char)flags;
    return box(type, b + body);
}

struct ItemSpec {
    int id;
    std::string type;               // "hvc1", "av01", "grid"
    std::string data;
    std::string config;             // hvcC / av1C contents ("" for a grid)
    int w = 0, h = 0;
    bool hidden = false;
    std::vector<std::string> extra; // more property boxes (whole boxes: irot, imir, colr, auxC...), associated after the others
};

// Builds the file; 'refs' are (type, from, to...) lists.
std::string heif(const char* brand, int primary, const std::vector<ItemSpec>& items,
                 const std::vector<std::vector<int>>& dimg, const std::vector<std::pair<int, int>>& auxl)
{
    std::string ftyp = box("ftyp", std::string(brand) + std::string(4, '\0') + "mif1" + brand);
    std::string hdlr = fullbox("hdlr", 0, 0, std::string(4, '\0') + "pict" + std::string(12, '\0') + std::string(1, '\0'));
    std::string pitm = fullbox("pitm", 0, 0, std::string(1, '\0') + std::string(1, (char)primary));
    std::string iinf;
    put16(iinf, (unsigned)items.size());
    for (const ItemSpec& it : items) {
        std::string e;
        put16(e, (unsigned)it.id);
        put16(e, 0);
        e += it.type;
        e += '\0';
        iinf += fullbox("infe", 2, it.hidden ? 1 : 0, e);
    }
    iinf = fullbox("iinf", 0, 0, iinf);
    // properties
    std::string ipco, ipma;
    std::vector<std::vector<int>> assoc(items.size());
    int propCount = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const ItemSpec& it = items[i];
        if (it.w) {
            std::string s;
            put32(s, (unsigned)it.w);
            put32(s, (unsigned)it.h);
            ipco += fullbox("ispe", 0, 0, s);
            assoc[i].push_back(++propCount);
        }
        if (!it.config.empty()) {
            ipco += box(it.type == "av01" ? "av1C" : "hvcC", it.config);
            assoc[i].push_back(++propCount);
        }
        for (const std::string& x : it.extra) {
            ipco += x;
            assoc[i].push_back(++propCount);
        }
    }
    std::string m;
    put32(m, (unsigned)items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        put16(m, (unsigned)items[i].id);
        m += (char)assoc[i].size();
        for (int a : assoc[i]) m += (char)(0x80 | a);
    }
    std::string iprp = box("iprp", box("ipco", ipco) + fullbox("ipma", 0, 0, m));
    // references
    std::string iref;
    for (const auto& d : dimg) {                    // d[0] = the grid, the rest its tiles
        std::string r;
        put16(r, (unsigned)d[0]);
        put16(r, (unsigned)d.size() - 1);
        for (size_t k = 1; k < d.size(); ++k) put16(r, (unsigned)d[k]);
        iref += box("dimg", r);
    }
    for (const auto& a : auxl) {                    // a.first = the alpha item, a.second = the picture
        std::string r;
        put16(r, (unsigned)a.first);
        put16(r, 1);
        put16(r, (unsigned)a.second);
        iref += box("auxl", r);
    }
    if (!iref.empty()) iref = fullbox("iref", 0, 0, iref);
    // iloc with absolute offsets into mdat; the sizes of everything before mdat are known after one pass
    auto build = [&](size_t mdatStart) {
        std::string iloc;
        iloc += (char)0x44;
        iloc += (char)0x00;
        put16(iloc, (unsigned)items.size());
        size_t off = mdatStart + 8;
        for (const ItemSpec& it : items) {
            put16(iloc, (unsigned)it.id);
            put16(iloc, 0);
            put16(iloc, 1);
            put32(iloc, (unsigned)off);
            put32(iloc, (unsigned)it.data.size());
            off += it.data.size();
        }
        return fullbox("iloc", 0, 0, iloc);
    };
    std::string meta = fullbox("meta", 0, 0, hdlr + pitm + build(0) + iinf + iprp + iref);
    const size_t mdatStart = ftyp.size() + meta.size();
    meta = fullbox("meta", 0, 0, hdlr + pitm + build(mdatStart) + iinf + iprp + iref);
    std::string mdat;
    for (const ItemSpec& it : items) mdat += it.data;
    return ftyp + meta + box("mdat", mdat);
}

std::string imir(int axis) { return box("imir", std::string(1, (char)axis)); }
std::string irot(int turns) { return box("irot", std::string(1, (char)turns)); }
std::string colr()
{
    std::string s = "nclx";
    put16(s, 1);
    put16(s, 13);
    put16(s, 1);
    s += (char)0x80;
    return box("colr", s);
}
std::string auxc() { return fullbox("auxC", 0, 0, std::string("urn:mpeg:mpegB:cicp:systems:auxiliary:alpha") + '\0'); }

bool write(const std::string& path, const std::string& bytes)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

// The pixel (x, y) of a frame from the top-left, un-premultiplied: B G R A.
struct Px { int b, g, r, a; };
Px pixel(const Image& img, int x, int y)
{
    const uint8_t* p = img.row(img.h - 1 - y) + x * 4;
    Px q = {p[0], p[1], p[2], p[3]};
    if (q.a > 0 && q.a < 255) {
        q.b = q.b * 255 / q.a;
        q.g = q.g * 255 / q.a;
        q.r = q.r * 255 / q.a;
    }
    return q;
}
bool near(int a, int b, int tol) { return a >= b - tol && a <= b + tol; }
bool gray(const Px& p, int level, int tol = 14) { return near(p.r, level, tol) && near(p.g, level, tol) && near(p.b, level, tol); }

void testCodec(const char* name, const char* encoder, AVCodecID id, const char* brand, const std::string& dir)
{
    const std::string fourcc = id == AV_CODEC_ID_HEVC ? "hvc1" : "av01";
    Coded white, dark, quad, red, alpha;
    if (!encode(encoder, id, 64, 64, 235, 128, 128, false, white) || !encode(encoder, id, 64, 64, 20, 128, 128, false, dark) ||
        !encode(encoder, id, 64, 64, 0, 128, 128, true, quad) || !encode(encoder, id, 64, 64, 54, 99, 255, false, red)) {
        std::printf("SKIP %s: no %s encoder here\n", name, encoder);
        ++gSkipped;
        return;
    }
    const std::string tag = std::string(name) + ": ";
    std::string err;

    // ---- one picture, with its colour
    {
        ItemSpec it{1, fourcc, red.data, red.config, 64, 64, false, {colr()}};
        const std::string path = dir + "/one_" + name + ".heic";
        write(path, heif(brand, 1, {it}, {}, {}));
        std::unique_ptr<MediaReader> r = openMedia(path, &err);
        check(r != nullptr, tag + "a single picture opens (" + err + ")");
        if (r) {
            const MediaInfo& mi = r->info();
            check(mi.hasVideo && mi.isStill && !mi.hasAudio && mi.width == 64 && mi.height == 64, tag + "it is a 64x64 still with no sound");
            ImagePtr img = r->videoFrame(0, 0, 0);
            check(img && img->w == 64 && img->h == 64, tag + "its frame has the picture's size");
            if (img) {
                const Px p = pixel(*img, 32, 32);
                check(p.r > 200 && p.g < 80 && p.b < 80 && p.a == 255, tag + "its colour is red (" + std::to_string(p.r) + "," + std::to_string(p.g) + "," + std::to_string(p.b) + ")");
            }
            ImagePtr small = r->videoFrame(0, 32, 32);
            check(small && small->w == 32 && small->h == 32, tag + "a smaller frame is scaled");
        }
    }
    // ---- a 2x2 grid cropped to 100x90: tiles white, dark, dark, white
    {
        std::string g;
        g += '\0';
        g += '\0';
        g += (char)1;
        g += (char)1;
        put16(g, 100);
        put16(g, 90);
        std::vector<ItemSpec> items;
        items.push_back(ItemSpec{10, "grid", g, "", 100, 90, false, {}});
        items.push_back(ItemSpec{11, fourcc, white.data, white.config, 64, 64, true, {}});
        items.push_back(ItemSpec{12, fourcc, dark.data, dark.config, 64, 64, true, {}});
        items.push_back(ItemSpec{13, fourcc, dark.data, dark.config, 64, 64, true, {}});
        items.push_back(ItemSpec{14, fourcc, white.data, white.config, 64, 64, true, {}});
        for (ItemSpec& s : items) s.extra.push_back(colr());
        // a grid has no codec configuration of its own: its colr stays with its tiles
        items[0].extra.clear();
        const std::string path = dir + "/grid_" + name + ".heic";
        write(path, heif(brand, 10, items, {{10, 11, 12, 13, 14}}, {}));
        std::unique_ptr<MediaReader> r = openMedia(path, &err);
        check(r != nullptr, tag + "a grid opens (" + err + ")");
        if (r) {
            check(r->info().width == 100 && r->info().height == 90, tag + "the grid has its output size 100x90");
            ImagePtr img = r->videoFrame(0, 0, 0);
            check(img && img->w == 100 && img->h == 90, tag + "its frame is 100x90");
            if (img && img->w == 100 && img->h == 90) {
                check(gray(pixel(*img, 20, 20), 235), tag + "tile (0,0) is white");
                check(gray(pixel(*img, 80, 20), 20), tag + "tile (1,0) is dark");
                check(gray(pixel(*img, 20, 80), 20), tag + "tile (0,1) is dark");
                check(gray(pixel(*img, 90, 80), 235), tag + "tile (1,1) is white and cropped");
            }
        }
    }
    // ---- rotation and mirror: bright top-left quarter, turned 90 degrees anticlockwise (bottom left), then mirrored left-right (bottom right)
    {
        ItemSpec it{1, fourcc, quad.data, quad.config, 64, 64, false, {colr(), irot(1)}};
        const std::string path = dir + "/rot_" + name + ".heic";
        write(path, heif(brand, 1, {it}, {}, {}));
        std::unique_ptr<MediaReader> r = openMedia(path, &err);
        check(r != nullptr, tag + "a turned picture opens");
        if (r) {
            ImagePtr img = r->videoFrame(0, 0, 0);
            check(r->info().rotation == 270, tag + "the file's turn is reported (270 clockwise)");
            if (img) {
                check(gray(pixel(*img, 8, 56), 235) && gray(pixel(*img, 8, 8), 20) && gray(pixel(*img, 56, 56), 20), tag + "the bright quarter went to the bottom left");
            }
        }
        it.extra = {colr(), imir(0)};
        write(path, heif(brand, 1, {it}, {}, {}));
        r = openMedia(path, &err);
        if (r) {
            ImagePtr img = r->videoFrame(0, 0, 0);
            if (img) check(gray(pixel(*img, 56, 8), 235) && gray(pixel(*img, 8, 8), 20), tag + "mirrored left to right the bright quarter is at the top right");
        }
        it.extra = {colr(), imir(1)};
        write(path, heif(brand, 1, {it}, {}, {}));
        r = openMedia(path, &err);
        if (r) {
            ImagePtr img = r->videoFrame(0, 0, 0);
            if (img) check(gray(pixel(*img, 8, 56), 235) && gray(pixel(*img, 8, 8), 20), tag + "mirrored top to bottom the bright quarter is at the bottom left");
        }
    }
    // ---- an alpha picture: opaque on the left half... here the whole alpha picture is the quadrant image (bright = opaque)
    {
        ItemSpec pic{1, fourcc, red.data, red.config, 64, 64, false, {colr()}};
        ItemSpec al{2, fourcc, quad.data, quad.config, 64, 64, true, {auxc()}};
        const std::string path = dir + "/alpha_" + name + ".heic";
        write(path, heif(brand, 1, {pic, al}, {}, {{2, 1}}));
        std::unique_ptr<MediaReader> r = openMedia(path, &err);
        check(r != nullptr && r->info().hasAlpha, tag + "an alpha picture is found");
        if (r) {
            ImagePtr img = r->videoFrame(0, 0, 0);
            if (img) {
                const Px a = pixel(*img, 8, 8), b = pixel(*img, 56, 56);
                check(a.a > 225 && b.a < 40, tag + "the alpha picture sets the transparency (" + std::to_string(a.a) + ", " + std::to_string(b.a) + ")");
                check(a.r > 200 && a.g < 80, tag + "the colour stays red where it shows");
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    testCodec("hevc", "libx265", AV_CODEC_ID_HEVC, "heic", dir);
    testCodec("av1", "libaom-av1", AV_CODEC_ID_AV1, "avif", dir);
    // a file that is not a HEIF file, or is a broken one, is not taken for one
    {
        const std::string path = dir + "/notheif.heic";
        write(path, box("ftyp", "heic" + std::string(4, '\0') + "mif1") + box("meta", std::string(8, '\0')));
        std::string err;
        std::unique_ptr<MediaReader> r = openMedia(path, &err);
        check(r == nullptr, "a HEIF file with nothing in it does not open (and does not crash)");
        write(path, "ftypheic");
        r = openMedia(path, &err);
        check(r == nullptr, "a cut-off file does not open");
    }
    std::printf("%d passed, %d failed, %d skipped\n", gPassed, gFailed, gSkipped);
    return gFailed ? 1 : 0;
}
