// HEIC / HEIF / AVIF still pictures, read by Reno itself. FFmpeg 7.1 and later read the simple ones (one coded picture)
// but hand out a grid image (every iPhone photo: the picture is a grid of tiles) as a pile of separate tiles, and
// FFmpeg 7.0 and earlier read none. Here the container is parsed (ISO base media: meta, iinf, iloc, iref, iprp), the
// coded pictures are decoded with FFmpeg's HEVC / AV1 / JPEG decoders, grids are put together, an alpha picture
// (AVIF, HEIC auxiliary image) becomes the transparency, and the rotation, mirror and crop properties are applied.
// Files this does not understand (image sequences, overlays, other codecs) are left to FFmpeg.
#include "ffmpeg_common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

namespace reno {

namespace {

using namespace ff;

uint32_t be16(const uint8_t* p) { return (uint32_t)(p[0] << 8) | p[1]; }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
uint32_t cc(const char* s) { return be32((const uint8_t*)s); }

struct Box {
    uint32_t type = 0;
    size_t body = 0, end = 0;       // the box's contents [body, end)
};

// The boxes in [at, end) of the file.
std::vector<Box> boxesIn(const std::string& file, size_t at, size_t end)
{
    std::vector<Box> v;
    const uint8_t* d = (const uint8_t*)file.data();
    end = std::min(end, file.size());
    while (at + 8 <= end) {
        uint64_t size = be32(d + at);
        Box b;
        b.type = be32(d + at + 4);
        size_t hdr = 8;
        if (size == 1) {
            if (at + 16 > end) break;
            size = be64(d + at + 8);
            hdr = 16;
        } else if (size == 0) {
            size = end - at;
        }
        if (size < hdr || size > end - at) break;
        b.body = at + hdr;
        b.end = at + (size_t)size;
        v.push_back(b);
        at = b.end;
    }
    return v;
}

struct Transform {
    uint32_t type = 0;              // 'irot', 'imir', 'clap'
    int a = 0;                      // irot: angle in quarter turns anticlockwise; imir: axis
    double cw = 0, ch = 0, ox = 0, oy = 0;      // clap: the clean aperture's size and its centre's offset
};

struct Prop {
    uint32_t type = 0;
    std::string data;               // the box contents
};

struct Extent { uint64_t offset = 0, length = 0; };
struct Item {
    uint32_t id = 0, type = 0;
    int method = 0;                 // iloc construction method: 0 file, 1 idat
    uint64_t baseOffset = 0;
    std::vector<Extent> extents;
    std::vector<int> props;         // indexes (1-based) into the property list, in order
    std::string contentType;
};

struct Colr {
    bool have = false;
    int matrix = 2;
    bool full = true;
};

struct Picture {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;      // straight B G R A, top row first
};

class HeifFile {
public:
    bool parse(const std::string& file);
    bool render(Picture& out, std::string* error);
    std::string codecName_;
    bool hasAlpha_ = false;
    int rotationCw_ = 0;            // the file's rotation, clockwise degrees (already applied to the picture)

private:
    const uint8_t* itemData(const Item& it, std::string& buf) const;
    bool decodeCoded(const Item& it, FramePtr& frame, std::string* error) const;
    bool toBgra(const Item& it, const AVFrame* frame, Picture& pic, bool alphaOnly) const;
    bool renderItem(uint32_t id, Picture& pic, std::string* error, int depth);
    const Prop* prop(const Item& it, uint32_t type) const;
    Colr colrOf(const Item& it) const;
    void applyTransforms(const Item& it, Picture& pic);

    std::string file_;
    uint32_t primary_ = 0;
    std::map<uint32_t, Item> items_;
    std::vector<Prop> props_;
    std::vector<std::tuple<uint32_t, uint32_t, std::vector<uint32_t>>> refs_;   // type, from, to...
    size_t idatBody_ = 0, idatEnd_ = 0;
};

bool HeifFile::parse(const std::string& file)
{
    file_ = file;
    const uint8_t* d = (const uint8_t*)file_.data();
    bool hasMoov = false;
    size_t metaBody = 0, metaEnd = 0;
    for (const Box& b : boxesIn(file_, 0, file_.size())) {
        if (b.type == cc("meta") && b.end - b.body > 4) {
            metaBody = b.body + 4;
            metaEnd = b.end;
        } else if (b.type == cc("moov")) {
            hasMoov = true;
        }
    }
    if (!metaEnd || hasMoov) return false;
    for (const Box& b : boxesIn(file_, metaBody, metaEnd)) {
        const size_t n = b.end - b.body;
        const uint8_t* p = d + b.body;
        if (b.type == cc("pitm") && n >= 6) {
            primary_ = p[0] == 0 ? be16(p + 4) : (n >= 8 ? be32(p + 4) : 0);
        } else if (b.type == cc("iinf") && n >= 6) {
            const int ver = p[0];
            size_t at = b.body + (ver == 0 ? 6 : 8);
            for (const Box& e : boxesIn(file_, at, b.end)) {
                if (e.type != cc("infe") || e.end - e.body < 12) continue;
                const uint8_t* q = d + e.body;
                const int v = q[0];
                if (v < 2) continue;
                Item it;
                size_t o = 4;
                if (v == 2) { it.id = be16(q + o); o += 2; } else { it.id = be32(q + o); o += 4; }
                o += 2;                                 // protection index
                if (o + 4 > e.end - e.body) continue;
                it.type = be32(q + o);
                if (it.type == cc("mime") && o + 5 < e.end - e.body) {
                    size_t s = o + 4;
                    while (s < e.end - e.body && q[s]) ++s;     // the name
                    ++s;
                    if (s < e.end - e.body) it.contentType.assign((const char*)q + s, strnlen((const char*)q + s, e.end - e.body - s));
                }
                Item& dst = items_[it.id];          // (iloc and iprp may have come first)
                dst.id = it.id;
                dst.type = it.type;
                dst.contentType = it.contentType;
            }
        } else if (b.type == cc("iloc") && n >= 8) {
            const int ver = p[0];
            const int offSize = p[4] >> 4, lenSize = p[4] & 15, baseSize = p[5] >> 4, idxSize = ver > 0 ? (p[5] & 15) : 0;
            size_t o = 6;
            uint32_t count = 0;
            if (ver < 2) { count = be16(p + o); o += 2; } else { if (n < 10) continue; count = be32(p + o); o += 4; }
            auto rd = [&](int size) -> uint64_t {
                uint64_t v = 0;
                if (o + (size_t)size > n) { o = n + 1; return 0; }
                for (int k = 0; k < size; ++k) v = (v << 8) | p[o + (size_t)k];
                o += (size_t)size;
                return v;
            };
            for (uint32_t i = 0; i < count && o <= n; ++i) {
                const uint32_t id = (uint32_t)rd(ver < 2 ? 2 : 4);
                int method = 0;
                if (ver > 0) method = (int)(rd(2) & 15);
                rd(2);                                  // data reference index
                const uint64_t base = rd(baseSize);
                const uint32_t ext = (uint32_t)rd(2);
                std::vector<Extent> ex;
                for (uint32_t k = 0; k < ext && o <= n; ++k) {
                    if (idxSize) rd(idxSize);
                    Extent e;
                    e.offset = rd(offSize);
                    e.length = rd(lenSize);
                    ex.push_back(e);
                }
                if (o > n) break;
                Item& it = items_[id];
                it.id = id;
                it.method = method;
                it.baseOffset = base;
                it.extents = ex;
            }
        } else if (b.type == cc("iref") && n >= 4) {
            const int ver = p[0];
            for (const Box& r : boxesIn(file_, b.body + 4, b.end)) {
                const uint8_t* q = d + r.body;
                const size_t rn = r.end - r.body;
                const size_t idSize = ver == 0 ? 2 : 4;
                if (rn < idSize + 2) continue;
                const uint32_t from = idSize == 2 ? be16(q) : be32(q);
                const uint32_t cnt = be16(q + idSize);
                std::vector<uint32_t> to;
                for (uint32_t k = 0; k < cnt && idSize + 2 + (k + 1) * idSize <= rn; ++k)
                    to.push_back(idSize == 2 ? be16(q + idSize + 2 + k * 2) : be32(q + idSize + 2 + k * 4));
                refs_.emplace_back(r.type, from, to);
            }
        } else if (b.type == cc("idat")) {
            idatBody_ = b.body;
            idatEnd_ = b.end;
        } else if (b.type == cc("iprp")) {
            for (const Box& c : boxesIn(file_, b.body, b.end)) {
                if (c.type == cc("ipco")) {
                    for (const Box& pr : boxesIn(file_, c.body, c.end)) {
                        Prop pp;
                        pp.type = pr.type;
                        pp.data.assign((const char*)d + pr.body, pr.end - pr.body);
                        props_.push_back(pp);
                    }
                }
            }
            for (const Box& c : boxesIn(file_, b.body, b.end)) {
                if (c.type != cc("ipma") || c.end - c.body < 8) continue;
                const uint8_t* q = d + c.body;
                const size_t cn = c.end - c.body;
                const int ver = q[0], flags = (q[1] << 16) | (q[2] << 8) | q[3];
                const uint32_t cnt = be32(q + 4);
                size_t o = 8;
                for (uint32_t i = 0; i < cnt; ++i) {
                    const size_t idSize = ver < 1 ? 2 : 4;
                    if (o + idSize + 1 > cn) break;
                    const uint32_t id = idSize == 2 ? be16(q + o) : be32(q + o);
                    o += idSize;
                    const int na = q[o++];
                    Item& it = items_[id];
                    it.id = id;
                    for (int k = 0; k < na; ++k) {
                        int idx;
                        if (flags & 1) {
                            if (o + 2 > cn) break;
                            idx = (int)(be16(q + o) & 0x7fff);
                            o += 2;
                        } else {
                            if (o + 1 > cn) break;
                            idx = q[o] & 0x7f;
                            o += 1;
                        }
                        it.props.push_back(idx);
                    }
                }
            }
        }
    }
    if (!primary_ || !items_.count(primary_)) return false;
    const uint32_t t = items_[primary_].type;
    return t == cc("hvc1") || t == cc("hev1") || t == cc("av01") || t == cc("grid") || t == cc("jpeg");
}

const Prop* HeifFile::prop(const Item& it, uint32_t type) const
{
    for (int idx : it.props)
        if (idx >= 1 && idx <= (int)props_.size() && props_[(size_t)idx - 1].type == type) return &props_[(size_t)idx - 1];
    return nullptr;
}

Colr HeifFile::colrOf(const Item& it) const
{
    Colr c;
    for (int idx : it.props) {
        if (idx < 1 || idx > (int)props_.size() || props_[(size_t)idx - 1].type != cc("colr")) continue;
        const std::string& s = props_[(size_t)idx - 1].data;
        if (s.size() >= 11 && !std::memcmp(s.data(), "nclx", 4)) {
            c.have = true;
            c.matrix = (int)be16((const uint8_t*)s.data() + 8);
            c.full = (s[10] & 0x80) != 0;
        }
    }
    return c;
}

const uint8_t* HeifFile::itemData(const Item& it, std::string& buf) const
{
    buf.clear();
    const size_t fsz = file_.size();
    for (const Extent& e : it.extents) {
        uint64_t off = it.baseOffset + e.offset;
        uint64_t len = e.length;
        size_t limit = fsz, origin = 0;
        if (it.method == 1) {
            origin = idatBody_;
            limit = idatEnd_;
            if (!idatEnd_) return nullptr;
        } else if (it.method != 0) {
            return nullptr;
        }
        if (len == 0) len = limit > origin + off ? limit - origin - off : 0;           // to the end
        if (origin + off > limit || len > limit - origin - off || buf.size() + len > (1u << 30)) return nullptr;
        buf.append(file_, origin + (size_t)off, (size_t)len);
    }
    return (const uint8_t*)buf.data();
}

bool HeifFile::decodeCoded(const Item& it, FramePtr& frame, std::string* error) const
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    AVCodecID id = AV_CODEC_ID_NONE;
    uint32_t cfgType = 0;
    if (it.type == cc("hvc1") || it.type == cc("hev1")) { id = AV_CODEC_ID_HEVC; cfgType = cc("hvcC"); }
    else if (it.type == cc("av01")) { id = AV_CODEC_ID_AV1; cfgType = cc("av1C"); }
    else if (it.type == cc("jpeg")) { id = AV_CODEC_ID_MJPEG; }
    else return fail("an unsupported picture type.");
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) return fail(strfmt("No decoder for %s in this FFmpeg.", avcodec_get_name(id)));
    std::string buf;
    const uint8_t* data = itemData(it, buf);
    if (!data || buf.empty()) return fail("the picture's data lies outside the file.");
    CodecPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) return fail("Out of memory.");
    if (cfgType) {
        const Prop* cfg = prop(it, cfgType);
        if (!cfg) return fail("the picture has no codec configuration.");
        ctx->extradata = (uint8_t*)av_mallocz(cfg->data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->extradata) return fail("Out of memory.");
        std::memcpy(ctx->extradata, cfg->data.data(), cfg->data.size());
        ctx->extradata_size = (int)cfg->data.size();
    }
    if (const Prop* sz = prop(it, cc("ispe"))) {
        if (sz->data.size() >= 12) {
            ctx->width = (int)be32((const uint8_t*)sz->data.data() + 4);
            ctx->height = (int)be32((const uint8_t*)sz->data.data() + 8);
        }
    }
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) return fail(strfmt("The %s decoder does not start.", codec->name));
    PacketPtr pkt(av_packet_alloc());
    FramePtr out(av_frame_alloc());
    if (!pkt || !out || av_new_packet(pkt.get(), (int)buf.size()) < 0) return fail("Out of memory.");
    std::memcpy(pkt->data, buf.data(), buf.size());
    int r = avcodec_send_packet(ctx.get(), pkt.get());
    if (r < 0) return fail("The picture cannot be decoded (" + errText(r) + ").");
    avcodec_send_packet(ctx.get(), nullptr);
    r = avcodec_receive_frame(ctx.get(), out.get());
    if (r < 0) return fail("The picture cannot be decoded (" + errText(r) + ").");
    frame = std::move(out);
    return true;
}

// The decoded frame as straight B G R A (alphaOnly: the first plane as a grey level in all four).
bool HeifFile::toBgra(const Item& it, const AVFrame* f, Picture& pic, bool alphaOnly) const
{
    const Colr colr = colrOf(it);
    const AVPixelFormat fmt = (AVPixelFormat)f->format;
    const int w = f->width, h = f->height;
    if (w < 1 || h < 1 || w > 32768 || h > 32768) return false;
    const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(fmt);
    if (!pd) return false;
    const bool rgb = (pd->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const AVPixelFormat dst = alphaOnly ? AV_PIX_FMT_GRAY8 : AV_PIX_FMT_BGRA;
    SwsPtr sws(sws_getContext(w, h, fmt, w, h, dst, SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, nullptr, nullptr, nullptr));
    if (!sws) return false;
    int space = f->colorspace;
    if (colr.have) {
        switch (colr.matrix) {
        case 1: space = AVCOL_SPC_BT709; break;
        case 5: space = AVCOL_SPC_BT470BG; break;
        case 6: space = AVCOL_SPC_SMPTE170M; break;
        case 7: space = AVCOL_SPC_SMPTE240M; break;
        case 9: case 10: space = AVCOL_SPC_BT2020_NCL; break;
        default: break;
        }
    }
    bool full = f->color_range == AVCOL_RANGE_JPEG || isJpegRangeFormat(fmt);
    if (f->color_range == AVCOL_RANGE_UNSPECIFIED && !isJpegRangeFormat(fmt)) full = colr.have ? colr.full : true;
    if (alphaOnly) full = f->color_range != AVCOL_RANGE_MPEG;
    if (!rgb) {
        const int* co = sws_getCoefficients(swsColorspace(space, w, h));
        sws_setColorspaceDetails(sws.get(), co, full ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    }
    const int bpp = alphaOnly ? 1 : 4;
    std::vector<uint8_t> out((size_t)w * (size_t)h * bpp + 64);
    uint8_t* dp[4] = {out.data(), nullptr, nullptr, nullptr};
    int ds[4] = {w * bpp, 0, 0, 0};
    if (sws_scale(sws.get(), f->data, f->linesize, 0, h, dp, ds) <= 0) return false;
    out.resize((size_t)w * (size_t)h * bpp);
    pic.w = w;
    pic.h = h;
    if (alphaOnly) {
        pic.bgra.resize((size_t)w * h * 4);
        for (size_t i = 0; i < (size_t)w * h; ++i) pic.bgra[i * 4] = pic.bgra[i * 4 + 1] = pic.bgra[i * 4 + 2] = pic.bgra[i * 4 + 3] = out[i];
    } else {
        pic.bgra.swap(out);
        for (size_t i = 3; i < pic.bgra.size(); i += 4) pic.bgra[i] = 255;
    }
    return true;
}

bool HeifFile::renderItem(uint32_t id, Picture& pic, std::string* error, int depth)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    auto found = items_.find(id);
    if (found == items_.end() || depth > 2) return fail("the picture is missing from the file.");
    const Item& it = found->second;
    if (it.type == cc("grid")) {
        std::string buf;
        const uint8_t* g = itemData(it, buf);
        if (!g || buf.size() < 8) return fail("the grid's description is missing.");
        const bool wide = (g[1] & 1) != 0;
        const int rows = g[2] + 1, cols = g[3] + 1;
        if (buf.size() < (wide ? 12u : 8u)) return fail("the grid's description is short.");
        const int ow = (int)(wide ? be32(g + 4) : be16(g + 4)), oh = (int)(wide ? be32(g + 8) : be16(g + 6));
        if (ow < 1 || oh < 1 || ow > 32768 || oh > 32768 || (int64_t)ow * oh > 400000000) return fail("the grid's size is not usable.");
        std::vector<uint32_t> tiles;
        for (const auto& r : refs_)
            if (std::get<0>(r) == cc("dimg") && std::get<1>(r) == id) tiles = std::get<2>(r);
        if ((int)tiles.size() != rows * cols) return fail("the grid's tiles are not all there.");
        pic.w = ow;
        pic.h = oh;
        pic.bgra.assign((size_t)ow * oh * 4, 0);
        int tw = 0, th = 0;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                Picture t;
                auto ti = items_.find(tiles[(size_t)r * cols + c]);
                if (ti == items_.end()) return fail("a tile is missing from the file.");
                FramePtr fr;
                if (!decodeCoded(ti->second, fr, error) || !toBgra(ti->second, fr.get(), t, false)) return error && !error->empty() ? false : fail("a tile cannot be decoded.");
                if (!tw) { tw = t.w; th = t.h; }
                const int x0 = c * tw, y0 = r * th;
                for (int y = 0; y < t.h && y0 + y < oh; ++y) {
                    if (x0 >= ow) break;
                    const int n = std::min(t.w, ow - x0);
                    std::memcpy(pic.bgra.data() + ((size_t)(y0 + y) * ow + x0) * 4, t.bgra.data() + (size_t)y * t.w * 4, (size_t)n * 4);
                }
            }
        }
        codecName_ = "hevc";
        {
            auto ti = items_.find(tiles[0]);
            if (ti != items_.end()) codecName_ = ti->second.type == cc("av01") ? "av1" : (ti->second.type == cc("jpeg") ? "mjpeg" : "hevc");
        }
        applyTransforms(it, pic);
        return true;
    }
    FramePtr fr;
    if (!decodeCoded(it, fr, error)) return false;
    if (!toBgra(it, fr.get(), pic, false)) return fail("the picture cannot be converted.");
    codecName_ = it.type == cc("av01") ? "av1" : (it.type == cc("jpeg") ? "mjpeg" : "hevc");
    // the stated size can be smaller than the coded one
    if (const Prop* sz = prop(it, cc("ispe"))) {
        if (sz->data.size() >= 12) {
            const int sw = (int)be32((const uint8_t*)sz->data.data() + 4), sh = (int)be32((const uint8_t*)sz->data.data() + 8);
            if (sw > 0 && sh > 0 && (sw < pic.w || sh < pic.h)) {
                const int nw = std::min(sw, pic.w), nh = std::min(sh, pic.h);
                std::vector<uint8_t> c((size_t)nw * nh * 4);
                for (int y = 0; y < nh; ++y) std::memcpy(c.data() + (size_t)y * nw * 4, pic.bgra.data() + (size_t)y * pic.w * 4, (size_t)nw * 4);
                pic.bgra.swap(c);
                pic.w = nw;
                pic.h = nh;
            }
        }
    }
    applyTransforms(it, pic);
    return true;
}

void HeifFile::applyTransforms(const Item& it, Picture& pic)
{
    for (int idx : it.props) {
        if (idx < 1 || idx > (int)props_.size()) continue;
        const Prop& p = props_[(size_t)idx - 1];
        const uint8_t* q = (const uint8_t*)p.data.data();
        if (p.type == cc("clap") && p.data.size() >= 32) {
            auto frac = [&](int k) { const uint32_t dd = be32(q + k * 8 + 4); return dd ? (double)(int32_t)be32(q + k * 8) / dd : 0.0; };
            const double cw = frac(0), ch = frac(1), ox = frac(2), oy = frac(3);
            const int w = (int)std::lround(cw), h = (int)std::lround(ch);
            if (w < 1 || h < 1 || w > pic.w || h > pic.h) continue;
            int x0 = (int)std::lround(ox + (pic.w - w) / 2.0), y0 = (int)std::lround(oy + (pic.h - h) / 2.0);
            x0 = std::max(0, std::min(pic.w - w, x0));
            y0 = std::max(0, std::min(pic.h - h, y0));
            std::vector<uint8_t> c((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) std::memcpy(c.data() + (size_t)y * w * 4, pic.bgra.data() + ((size_t)(y0 + y) * pic.w + x0) * 4, (size_t)w * 4);
            pic.bgra.swap(c);
            pic.w = w;
            pic.h = h;
        } else if (p.type == cc("irot") && !p.data.empty()) {
            const int turns = q[0] & 3;                                     // anticlockwise quarter turns
            rotationCw_ = (rotationCw_ + (4 - turns) * 90) % 360;
            if (!turns) continue;
            const int w = pic.w, h = pic.h;
            std::vector<uint8_t> c(pic.bgra.size());
            const int nw = (turns & 1) ? h : w, nh = (turns & 1) ? w : h;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int nx, ny;
                    if (turns == 1) { nx = y; ny = w - 1 - x; }              // 90 degrees anticlockwise
                    else if (turns == 2) { nx = w - 1 - x; ny = h - 1 - y; }
                    else { nx = h - 1 - y; ny = x; }
                    std::memcpy(c.data() + ((size_t)ny * nw + nx) * 4, pic.bgra.data() + ((size_t)y * w + x) * 4, 4);
                }
            }
            pic.bgra.swap(c);
            pic.w = nw;
            pic.h = nh;
        } else if (p.type == cc("imir") && !p.data.empty()) {
            const bool horizontalAxis = (q[0] & 1) != 0;                    // axis 1: top and bottom swap; 0: left and right
            if (horizontalAxis) {
                for (int y = 0; y < pic.h / 2; ++y)
                    for (size_t b = 0; b < (size_t)pic.w * 4; ++b) std::swap(pic.bgra[(size_t)y * pic.w * 4 + b], pic.bgra[(size_t)(pic.h - 1 - y) * pic.w * 4 + b]);
            } else {
                for (int y = 0; y < pic.h; ++y)
                    for (int x = 0; x < pic.w / 2; ++x)
                        for (int b = 0; b < 4; ++b) std::swap(pic.bgra[((size_t)y * pic.w + x) * 4 + b], pic.bgra[((size_t)y * pic.w + (pic.w - 1 - x)) * 4 + b]);
            }
        }
    }
}

bool HeifFile::render(Picture& out, std::string* error)
{
    rotationCw_ = 0;
    if (!renderItem(primary_, out, error, 0)) return false;
    // transparency: an auxiliary picture (alpha) pointing at the primary one
    for (const auto& r : refs_) {
        if (std::get<0>(r) != cc("auxl")) continue;
        const bool toPrimary = std::find(std::get<2>(r).begin(), std::get<2>(r).end(), primary_) != std::get<2>(r).end();
        auto ai = items_.find(std::get<1>(r));
        if (!toPrimary || ai == items_.end()) continue;
        const Prop* aux = prop(ai->second, cc("auxC"));
        if (!aux || aux->data.size() < 5) continue;
        const std::string urn = aux->data.substr(4);
        if (urn.find("alpha") == std::string::npos && urn.find("auxid:1") == std::string::npos) continue;
        FramePtr fr;
        Picture a;
        std::string e;
        if (!decodeCoded(ai->second, fr, &e) || !toBgra(ai->second, fr.get(), a, true)) continue;
        // it is laid over the picture before the picture's own transforms; the primary's transforms were applied already, so
        // apply the same to the alpha
        Item same = ai->second;
        same.props = items_[primary_].props;
        const int rotation = rotationCw_;
        applyTransforms(same, a);
        rotationCw_ = rotation;             // (the picture's turn was counted once already)
        if (a.w != out.w || a.h != out.h) {
            SwsPtr s(sws_getContext(a.w, a.h, AV_PIX_FMT_BGRA, out.w, out.h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!s) continue;
            std::vector<uint8_t> sc((size_t)out.w * out.h * 4 + 64);
            const uint8_t* in[4] = {a.bgra.data(), nullptr, nullptr, nullptr};
            const int ins[4] = {a.w * 4, 0, 0, 0};
            uint8_t* op[4] = {sc.data(), nullptr, nullptr, nullptr};
            int os[4] = {out.w * 4, 0, 0, 0};
            if (sws_scale(s.get(), in, ins, 0, a.h, op, os) <= 0) continue;
            sc.resize((size_t)out.w * out.h * 4);
            a.bgra.swap(sc);
            a.w = out.w;
            a.h = out.h;
        }
        for (size_t i = 0; i < (size_t)out.w * out.h; ++i) out.bgra[i * 4 + 3] = a.bgra[i * 4];
        hasAlpha_ = true;
        break;
    }
    return true;
}

class HeifReader final : public MediaReader {
public:
    bool open(const std::string& path, const std::string& bytes, std::string* error);
    const MediaInfo& info() const override { return info_; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override;
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        (void)start;
        (void)sampleRate;
        if (frames > 0) {
            std::memset(left, 0, (size_t)frames * sizeof(float));
            std::memset(right, 0, (size_t)frames * sizeof(float));
        }
        return false;
    }

private:
    MediaInfo info_;
    Picture pic_;
    struct Cached { int w, h; ImagePtr img; };
    std::vector<Cached> cache_;
};

bool HeifReader::open(const std::string& path, const std::string& bytes, std::string* error)
{
    HeifFile f;
    if (!f.parse(bytes)) return false;
    std::string e;
    if (!f.render(pic_, &e)) {
        if (error) *error = pathFileName(path) + ": " + e;
        return false;
    }
    info_.path = path;
    info_.hasVideo = true;
    info_.isStill = true;
    info_.width = pic_.w;
    info_.height = pic_.h;
    info_.videoCodec = f.codecName_;
    info_.hasAlpha = f.hasAlpha_;
    info_.rotation = f.rotationCw_;
    return true;
}

ImagePtr HeifReader::videoFrame(double seconds, int maxW, int maxH)
{
    (void)seconds;
    const int cw = pic_.w, ch = pic_.h;
    int ow = cw, oh = ch;
    if (maxW > 0 && maxH > 0) {
        const double s = std::min((double)std::min(maxW, 32768) / cw, (double)std::min(maxH, 32768) / ch);
        ow = std::max(1, std::min(maxW, (int)std::lround(cw * s)));
        oh = std::max(1, std::min(maxH, (int)std::lround(ch * s)));
    }
    for (const Cached& c : cache_)
        if (c.w == ow && c.h == oh) return c.img;
    ImagePtr img = Image::make(ow, oh, PixelDepth::Byte, false);
    if (!img) return nullptr;
    const uint8_t* src = pic_.bgra.data();
    int srcStride = cw * 4;
    std::vector<uint8_t> scaled;
    if (ow != cw || oh != ch) {
        SwsPtr s(sws_getContext(cw, ch, AV_PIX_FMT_BGRA, ow, oh, AV_PIX_FMT_BGRA, std::min((double)ow / cw, (double)oh / ch) <= 0.5 ? SWS_AREA : SWS_BILINEAR,
                                nullptr, nullptr, nullptr));
        if (!s) return nullptr;
        const int stride = FFALIGN(ow * 4, 64);
        scaled.resize((size_t)stride * oh + 64);
        const uint8_t* in[4] = {pic_.bgra.data(), nullptr, nullptr, nullptr};
        const int inStride[4] = {cw * 4, 0, 0, 0};
        uint8_t* outp[4] = {scaled.data(), nullptr, nullptr, nullptr};
        int outStride[4] = {stride, 0, 0, 0};
        if (sws_scale(s.get(), in, inStride, 0, ch, outp, outStride) <= 0) return nullptr;
        src = scaled.data();
        srcStride = stride;
    }
    for (int y = 0; y < oh; ++y) {
        const uint8_t* s = src + (size_t)y * srcStride;
        uint8_t* d = img->row(oh - 1 - y);
        for (int x = 0; x < ow; ++x, s += 4, d += 4) {
            const unsigned a = s[3];
            d[0] = (uint8_t)((s[0] * a + 127) / 255);
            d[1] = (uint8_t)((s[1] * a + 127) / 255);
            d[2] = (uint8_t)((s[2] * a + 127) / 255);
            d[3] = (uint8_t)a;
        }
    }
    cache_.push_back({ow, oh, img});
    if (cache_.size() > 16) cache_.erase(cache_.begin());
    return img;
}

}  // namespace

// Whether 'path' looks like a HEIF file (a 'ftyp' box with a HEIF brand); its bytes are read into *bytes when it does.
bool isHeifStill(const std::string& path, std::string* bytes)
{
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(widen(path).c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    uint8_t head[64] = {0};
    const size_t got = std::fread(head, 1, sizeof(head), f);
    bool heif = false;
    if (got >= 16 && !std::memcmp(head + 4, "ftyp", 4)) {
        const size_t size = std::min<size_t>(be32(head), got);
        static const char* const brands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "avif", "msf1", nullptr};
        for (size_t at = 8; at + 4 <= size && !heif; at += (at == 8 ? 8 : 4))      // major brand, then (after the version) the compatible ones
            for (const char* const* b = brands; *b; ++b)
                if (!std::memcmp(head + at, *b, 4)) heif = true;
    }
    if (heif && bytes) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        heif = false;
        if (size > 0 && size < (1l << 29)) {
            bytes->resize((size_t)size);
            bytes->resize(std::fread(&(*bytes)[0], 1, (size_t)size, f));
            heif = true;
        }
    }
    std::fclose(f);
    return heif;
}

std::unique_ptr<MediaReader> openHeifStill(const std::string& path, const std::string& bytes, std::string* error)
{
    std::unique_ptr<HeifReader> r(new HeifReader());
    if (!r->open(path, bytes, error)) return nullptr;
    return std::unique_ptr<MediaReader>(r.release());
}

}  // namespace reno
