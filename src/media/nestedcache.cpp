// Preview files of nested projects: see nestedcache.h.
#include "nestedcache.h"
#include "ffmpeg_common.h"
#include "vegasfiles.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace reno {

namespace {
const char kMagic[4] = {'R', 'N', 'P', 'V'};
const uint32_t kVersion = 1;
const size_t kHeader = 64;

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

FILE* openFile(const std::string& path, const char* mode)
{
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void removeFile(const std::string& path)
{
#ifdef _WIN32
    DeleteFileW(widen(path).c_str());
#else
    std::remove(path.c_str());
#endif
}

bool replaceFile(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// FNV-1a of the project's folder, so that two projects of the same name in different folders get different files.
uint32_t fnv32(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

// The header, and the codec's tag (UT Video says which of its kinds it is by the tag alone).
void makeHeader(uint8_t* h, const NestedCacheInfo& info, uint32_t extradata, uint64_t indexAt, uint32_t tag)
{
    std::memset(h, 0, kHeader);
    std::memcpy(h, kMagic, 4);
    put32(h + 4, kVersion);
    put32(h + 8, (uint32_t)info.width);
    put32(h + 12, (uint32_t)info.height);
    put32(h + 16, (uint32_t)info.fpsNum);
    put32(h + 20, (uint32_t)info.fpsDen);
    put64(h + 24, (uint64_t)info.frames);
    put64(h + 32, info.signature);
    put32(h + 40, (uint32_t)info.codec);
    put32(h + 44, extradata);
    put64(h + 48, indexAt);
    put32(h + 56, tag);
}

bool parseHeader(const uint8_t* h, NestedCacheInfo& info, uint32_t& extradata, uint64_t& indexAt, uint32_t& tag)
{
    if (std::memcmp(h, kMagic, 4) != 0 || get32(h + 4) != kVersion) return false;
    info.width = (int)get32(h + 8);
    info.height = (int)get32(h + 12);
    info.fpsNum = (int)get32(h + 16);
    info.fpsDen = (int)get32(h + 20);
    info.frames = (int64_t)get64(h + 24);
    info.signature = get64(h + 32);
    info.codec = (int)get32(h + 40);
    extradata = get32(h + 44);
    indexAt = get64(h + 48);
    tag = get32(h + 56);
    // an unfinished file has no index yet
    return info.width > 0 && info.height > 0 && info.frames >= 0 && indexAt >= kHeader && (info.codec == 0 || info.codec == 1);
}
}  // namespace

std::string nestedCachePath(const std::string& projectPath) { return projectPath + ".renopreview"; }

std::string nestedCacheFallbackPath(const std::string& projectPath)
{
    std::string dir, name = pathFileName(projectPath);
    const size_t slash = projectPath.find_last_of("/\\");
    if (slash != std::string::npos) dir = projectPath.substr(0, slash);
    return pathJoin(pathJoin(pathJoin(userDataDir(), "cache"), "nested"), strfmt("%08x_", fnv32(toLower(dir))) + name + ".renopreview");
}

bool readNestedCacheInfo(const std::string& path, NestedCacheInfo& out)
{
    FILE* f = openFile(path, "rb");
    if (!f) return false;
    uint8_t h[kHeader];
    const bool got = std::fread(h, 1, kHeader, f) == kHeader;
    std::fclose(f);
    uint32_t extradata = 0, tag = 0;
    uint64_t indexAt = 0;
    NestedCacheInfo info;
    if (!got || !parseHeader(h, info, extradata, indexAt, tag)) return false;
    out = info;
    return true;
}

// ====================================================================== writing
struct NestedCacheWriter::Encoder {
    ff::CodecPtr ctx;
    ff::FramePtr frame;
    ff::PacketPtr pkt;
};

NestedCacheWriter::NestedCacheWriter() = default;
NestedCacheWriter::~NestedCacheWriter() { abort(); }

bool NestedCacheWriter::open(const std::string& path, const NestedCacheInfo& info, std::string* error)
{
    abort();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        abort();
        return false;
    };
    if (info.width <= 0 || info.height <= 0) return fail("the frames have no size");
    path_ = path;
    part_ = path + ".part";
    info_ = info;
    index_.clear();
    extradata_.clear();
    try {
        const std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
        std::error_code ec;
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    } catch (...) {
    }
    FILE* f = openFile(part_, "wb");
    if (!f) return fail("cannot write " + pathFileName(part_));
    file_ = f;
    // UT Video when FFmpeg has its encoder: one picture at a time, without loss, as planes G, B, R, A
    info_.codec = 0;
    uint32_t tag = 0;
    if (const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_UTVIDEO)) {
        std::unique_ptr<Encoder> e(new Encoder());
        e->ctx.reset(avcodec_alloc_context3(c));
        e->frame.reset(av_frame_alloc());
        e->pkt.reset(av_packet_alloc());
        if (e->ctx && e->frame && e->pkt) {
            e->ctx->width = info.width;
            e->ctx->height = info.height;
            e->ctx->pix_fmt = AV_PIX_FMT_GBRAP;
            e->ctx->time_base = av_make_q(std::max(1, info.fpsDen), std::max(1, info.fpsNum));
            e->ctx->thread_count = 1;               // (one in, one out at once)
            // in bands, which reading decodes on several cores at once (in one piece a frame took 3 ms to read)
            e->ctx->slices = std::max(1, std::min(16, info.height / 32));
            e->frame->format = AV_PIX_FMT_GBRAP;
            e->frame->width = info.width;
            e->frame->height = info.height;
            if (avcodec_open2(e->ctx.get(), c, nullptr) >= 0 && av_frame_get_buffer(e->frame.get(), 0) >= 0) {
                if (e->ctx->extradata && e->ctx->extradata_size > 0) extradata_.assign((const char*)e->ctx->extradata, (size_t)e->ctx->extradata_size);
                tag = e->ctx->codec_tag;
                info_.codec = 1;
                enc_ = std::move(e);
            }
        }
    }
    uint8_t h[kHeader];
    makeHeader(h, info_, (uint32_t)extradata_.size(), 0, tag);
    if (std::fwrite(h, 1, kHeader, f) != kHeader) return fail("cannot write " + pathFileName(part_));
    if (!extradata_.empty() && std::fwrite(extradata_.data(), 1, extradata_.size(), f) != extradata_.size()) return fail("cannot write " + pathFileName(part_));
    at_ = kHeader + extradata_.size();
    return true;
}

bool NestedCacheWriter::write(const Image& img)
{
    FILE* f = (FILE*)file_;
    if (!f || img.depth != PixelDepth::Byte || img.w != info_.width || img.h != info_.height) return false;
    img.sync();
    const int w = img.w, hgt = img.h;
    const uint8_t* bytes = nullptr;
    size_t n = 0;
    if (enc_) {
        AVFrame* fr = enc_->frame.get();
        if (av_frame_make_writable(fr) < 0) return false;
        for (int y = 0; y < hgt; ++y) {
            const uint8_t* s = img.row(y);
            uint8_t* g = fr->data[0] + (size_t)y * fr->linesize[0];
            uint8_t* b = fr->data[1] + (size_t)y * fr->linesize[1];
            uint8_t* r = fr->data[2] + (size_t)y * fr->linesize[2];
            uint8_t* a = fr->data[3] + (size_t)y * fr->linesize[3];
            for (int x = 0; x < w; ++x, s += 4) {
                b[x] = s[0];
                g[x] = s[1];
                r[x] = s[2];
                a[x] = s[3];
            }
        }
        fr->pts = (int64_t)index_.size();
        if (avcodec_send_frame(enc_->ctx.get(), fr) < 0) return false;
        AVPacket* pkt = enc_->pkt.get();
        if (avcodec_receive_packet(enc_->ctx.get(), pkt) < 0) return false;
        buf_.assign(pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
        bytes = buf_.data();
        n = buf_.size();
    } else {
        buf_.resize((size_t)w * 4 * (size_t)hgt);
        for (int y = 0; y < hgt; ++y) std::memcpy(buf_.data() + (size_t)y * w * 4, img.row(y), (size_t)w * 4);
        bytes = buf_.data();
        n = buf_.size();
    }
    if (std::fwrite(bytes, 1, n, f) != n) return false;
    index_.emplace_back(at_, (uint32_t)n);
    at_ += n;
    return true;
}

bool NestedCacheWriter::finish(std::string* error)
{
    FILE* f = (FILE*)file_;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        abort();
        return false;
    };
    if (!f) return fail("nothing was written");
    info_.frames = (int64_t)index_.size();
    std::vector<uint8_t> ix(index_.size() * 16, 0);
    for (size_t k = 0; k < index_.size(); ++k) {
        put64(ix.data() + k * 16, index_[k].first);
        put32(ix.data() + k * 16 + 8, index_[k].second);
    }
    bool ok = ix.empty() || std::fwrite(ix.data(), 1, ix.size(), f) == ix.size();
    uint8_t h[kHeader];
    makeHeader(h, info_, (uint32_t)extradata_.size(), at_, enc_ ? enc_->ctx->codec_tag : 0);
    ok = ok && std::fflush(f) == 0 && std::fseek(f, 0, SEEK_SET) == 0 && std::fwrite(h, 1, kHeader, f) == kHeader;
    ok = (std::fclose(f) == 0) && ok;
    file_ = nullptr;
    enc_.reset();
    if (!ok) {
        removeFile(part_);
        return fail("cannot write " + pathFileName(part_) + " (is the disk full?)");
    }
    if (!replaceFile(part_, path_)) {
        removeFile(part_);
        return fail("cannot replace " + pathFileName(path_) + " (is it in use?)");
    }
    return true;
}

void NestedCacheWriter::abort()
{
    if (file_) {
        std::fclose((FILE*)file_);
        file_ = nullptr;
        removeFile(part_);
    }
    enc_.reset();
}

// ====================================================================== reading
struct NestedCacheFile::Decoder {
    ff::CodecPtr ctx;
    ff::FramePtr frame;
    ff::PacketPtr pkt;
};

NestedCacheFile::NestedCacheFile() = default;
NestedCacheFile::~NestedCacheFile() { close(); }

void NestedCacheFile::close()
{
    std::lock_guard<std::mutex> lk(mutex_);
    idle_.clear();
    if (!handle_) return;
#ifdef _WIN32
    CloseHandle((HANDLE)handle_);
#else
    ::close((int)(intptr_t)handle_ - 1);
#endif
    handle_ = nullptr;
}

bool NestedCacheFile::readAt(uint64_t offset, void* to, size_t n)
{
    uint8_t* p = (uint8_t*)to;
    while (n > 0) {
#ifdef _WIN32
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)(offset & 0xffffffff);
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        const DWORD want = (DWORD)std::min<size_t>(n, 1 << 30);
        if (!ReadFile((HANDLE)handle_, p, want, &got, &ov) || got == 0) return false;
#else
        const ssize_t got = ::pread((int)(intptr_t)handle_ - 1, p, n, (off_t)offset);
        if (got <= 0) return false;
#endif
        p += got;
        n -= (size_t)got;
        offset += (uint64_t)got;
    }
    return true;
}

bool NestedCacheFile::open(const std::string& path, std::string* error)
{
    close();
    std::lock_guard<std::mutex> lk(mutex_);
#ifdef _WIN32
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot be opened";
        return false;
    }
    handle_ = h;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error) *error = "cannot be opened";
        return false;
    }
    handle_ = (void*)(intptr_t)(fd + 1);
#endif
    auto failLocked = [&](const std::string& why) {
        if (error) *error = why;
#ifdef _WIN32
        CloseHandle((HANDLE)handle_);
#else
        ::close((int)(intptr_t)handle_ - 1);
#endif
        handle_ = nullptr;
        return false;
    };
    uint8_t hd[kHeader];
    uint32_t extradata = 0, tag = 0;
    uint64_t indexAt = 0;
    NestedCacheInfo info;
    if (!readAt(0, hd, kHeader) || !parseHeader(hd, info, extradata, indexAt, tag)) return failLocked("is not a finished preview file");
    extradata_.assign(extradata, '\0');
    if (extradata > 0 && !readAt(kHeader, &extradata_[0], extradata)) return failLocked("is cut short");
    std::vector<uint8_t> ix((size_t)info.frames * 16);
    if (!ix.empty() && !readAt(indexAt, ix.data(), ix.size())) return failLocked("is cut short");
    index_.resize((size_t)info.frames);
    for (size_t k = 0; k < index_.size(); ++k) index_[k] = {get64(ix.data() + k * 16), get32(ix.data() + k * 16 + 8)};
    path_ = path;
    info_ = info;
    tag_ = tag;
    idle_.clear();
    return true;
}

std::unique_ptr<NestedCacheFile::Decoder> NestedCacheFile::takeDecoder()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!idle_.empty()) {
            std::unique_ptr<Decoder> d = std::move(idle_.back());
            idle_.pop_back();
            return d;
        }
    }
    const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_UTVIDEO);
    if (!c) return nullptr;
    std::unique_ptr<Decoder> d(new Decoder());
    d->ctx.reset(avcodec_alloc_context3(c));
    d->frame.reset(av_frame_alloc());
    d->pkt.reset(av_packet_alloc());
    if (!d->ctx || !d->frame || !d->pkt) return nullptr;
    d->ctx->width = info_.width;
    d->ctx->height = info_.height;
    d->ctx->codec_tag = tag_;
    d->ctx->thread_type = FF_THREAD_SLICE;      // (slices of one picture at once; frame threads would hold pictures back)
    d->ctx->thread_count = 0;
    if (!extradata_.empty()) {
        d->ctx->extradata = (uint8_t*)av_mallocz(extradata_.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!d->ctx->extradata) return nullptr;
        std::memcpy(d->ctx->extradata, extradata_.data(), extradata_.size());
        d->ctx->extradata_size = (int)extradata_.size();
    }
    if (avcodec_open2(d->ctx.get(), c, nullptr) < 0) return nullptr;
    return d;
}

void NestedCacheFile::giveBack(std::unique_ptr<Decoder> d)
{
    if (!d) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (idle_.size() < 8) idle_.push_back(std::move(d));
}

ImagePtr NestedCacheFile::read(int64_t frame)
{
    if (frame < 0 || frame >= (int64_t)index_.size()) return nullptr;
    const uint64_t at = index_[(size_t)frame].first;
    const uint32_t n = index_[(size_t)frame].second;
    const int w = info_.width, h = info_.height;
    if (info_.codec == 0) {
        if (n != (uint32_t)w * 4 * (uint32_t)h) return nullptr;
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        std::vector<uint8_t> raw(n);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!handle_ || !readAt(at, raw.data(), n)) return nullptr;
        }
        for (int y = 0; y < h; ++y) std::memcpy(img->row(y), raw.data() + (size_t)y * w * 4, (size_t)w * 4);
        return img;
    }
    std::unique_ptr<Decoder> d = takeDecoder();
    if (!d) return nullptr;
    AVPacket* pkt = d->pkt.get();
    if (av_new_packet(pkt, (int)n) < 0) return nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!handle_ || !readAt(at, pkt->data, n)) {
            av_packet_unref(pkt);
            return nullptr;
        }
    }
    pkt->flags |= AV_PKT_FLAG_KEY;
    const int sent = avcodec_send_packet(d->ctx.get(), pkt);
    av_packet_unref(pkt);
    if (sent < 0) return nullptr;
    AVFrame* fr = d->frame.get();
    if (avcodec_receive_frame(d->ctx.get(), fr) < 0) return nullptr;
    ImagePtr img;
    if (fr->format == AV_PIX_FMT_GBRAP && fr->width == w && fr->height == h) {
        img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* o = img->row(y);
            const uint8_t* g = fr->data[0] + (size_t)y * fr->linesize[0];
            const uint8_t* b = fr->data[1] + (size_t)y * fr->linesize[1];
            const uint8_t* r = fr->data[2] + (size_t)y * fr->linesize[2];
            const uint8_t* a = fr->data[3] + (size_t)y * fr->linesize[3];
            for (int x = 0; x < w; ++x, o += 4) {
                o[0] = b[x];
                o[1] = g[x];
                o[2] = r[x];
                o[3] = a[x];
            }
        }
    }
    av_frame_unref(fr);
    giveBack(std::move(d));
    return img;
}

}  // namespace reno
