// Decoding video on the graphics card (see hwdecode.h): the settings, the choice of API, the shared devices, the limit on
// sessions and the copy of a frame from the card's memory.
#include "hwdecode.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

namespace reno {

namespace {
#ifdef _WIN32
const HwDecodeMode kDefaultMode = HwDecodeMode::Auto;       // as VEGAS
#else
const HwDecodeMode kDefaultMode = HwDecodeMode::Off;        // not tried on a card here
#endif
std::atomic<int> gMode{(int)kDefaultMode};
std::mutex gApiMutex;
std::string gApi;
std::atomic<int> gSessionLimit{8};
std::atomic<int> gSessions{0};

struct DeviceSlot {
    AVBufferRef* ref = nullptr;
    bool failed = false;
};
std::mutex gDeviceMutex;
std::map<int, DeviceSlot> gDevices;

// tests
std::atomic<bool> gEmulate{false}, gEmulateSemiPlanar{false};
std::atomic<int> gFailTransferAfter{-1}, gFailDecodeAfter{-1};
std::atomic<int> gTransferAttempts{0}, gDecodedEmulated{0};
std::atomic<long> gTransfers{0};
int gMark = 0;                                              // its address marks emulated card frames

void forgetFailedDevices()
{
    std::lock_guard<std::mutex> lock(gDeviceMutex);
    for (auto& d : gDevices) d.second.failed = false;
}
}  // namespace

void setHardwareDecoding(HwDecodeMode mode, const std::string& api)
{
    gMode.store((int)mode);
    {
        std::lock_guard<std::mutex> lock(gApiMutex);
        gApi = api == "auto" ? std::string() : api;
    }
    forgetFailedDevices();
}
HwDecodeMode hardwareDecoding() { return (HwDecodeMode)gMode.load(); }
std::string hardwareDecodingApi()
{
    std::lock_guard<std::mutex> lock(gApiMutex);
    return gApi;
}
void setHardwareDecoderLimit(int n) { gSessionLimit.store(std::max(1, std::min(64, n))); }
int hardwareDecoderLimit() { return gSessionLimit.load(); }

std::vector<std::string> hardwareDecodeApis()
{
    std::vector<std::string> out;
    for (AVHWDeviceType t : hw::candidateTypes(HwDecodeMode::Auto, std::string(), hw::currentPlatform(), hw::availableTypes()))
        out.push_back(hw::typeName(t));
    return out;
}

namespace hw {

Platform currentPlatform()
{
#if defined(_WIN32)
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::Mac;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Other;
#endif
}

const char* typeName(AVHWDeviceType t)
{
    const char* n = av_hwdevice_get_type_name(t);
    return n ? n : "";
}

AVHWDeviceType typeFromName(const std::string& name)
{
    return name.empty() ? AV_HWDEVICE_TYPE_NONE : av_hwdevice_find_type_by_name(name.c_str());
}

std::vector<AVHWDeviceType> availableTypes()
{
    std::vector<AVHWDeviceType> out;
    for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); t != AV_HWDEVICE_TYPE_NONE; t = av_hwdevice_iterate_types(t))
        out.push_back(t);
    return out;
}

std::vector<AVHWDeviceType> candidateTypes(HwDecodeMode mode, const std::string& api, Platform platform, const std::vector<AVHWDeviceType>& available)
{
    std::vector<AVHWDeviceType> out;
    if (mode == HwDecodeMode::Off) return out;
    auto has = [&](AVHWDeviceType t) { return std::find(available.begin(), available.end(), t) != available.end(); };
    if (!api.empty() && api != "auto") {
        const AVHWDeviceType t = typeFromName(api);
        if (t != AV_HWDEVICE_TYPE_NONE && has(t)) out.push_back(t);
        return out;
    }
    std::vector<AVHWDeviceType> order;
    switch (platform) {
    case Platform::Windows: order = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_CUDA}; break;
    case Platform::Linux:   order = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_VDPAU, AV_HWDEVICE_TYPE_CUDA}; break;
    case Platform::Mac:     order = {AV_HWDEVICE_TYPE_VIDEOTOOLBOX}; break;
    case Platform::Other:   break;
    }
    for (AVHWDeviceType t : order)
        if (has(t)) out.push_back(t);
    return out;
}

bool codecEligible(AVCodecID codec, int profile, int pixFormat, int width, int height, std::string* why)
{
    auto no = [&](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    int maxDepth = 8;
    switch (codec) {
    case AV_CODEC_ID_H264:
        if (profile == 110 || profile == 122 || profile == 144 || profile == 244 || profile == 44) return no("H.264 High 10 / 4:2:2 / 4:4:4 profiles are not supported by graphics cards");
        break;
    case AV_CODEC_ID_HEVC:
        maxDepth = 10;
        if (profile > 3) return no("HEVC range extensions profiles are not decoded by most graphics cards");
        break;
    case AV_CODEC_ID_VP9:
        maxDepth = 10;
        if (profile == 1 || profile == 3) return no("VP9 profiles 1 and 3 (4:4:4) are not decoded by graphics cards");
        break;
    case AV_CODEC_ID_AV1:
        maxDepth = 10;
        if (profile > 0) return no("only AV1's main profile is decoded by graphics cards");
        break;
    case AV_CODEC_ID_MPEG2VIDEO:
        if (profile == 0) return no("MPEG-2 4:2:2 profile is not decoded by graphics cards");
        break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
        break;
    default:
        return no(std::string(avcodec_get_name(codec)) + " has no graphics card decoder");
    }
    if (width < 64 || height < 64) return no("too small to be worth a graphics card");
    if (pixFormat >= 0) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)pixFormat);
        if (d) {
            if (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL))
                return no("not plain YUV");
            if (d->nb_components != 3 || d->log2_chroma_w != 1 || d->log2_chroma_h != 1) return no("not 4:2:0");
            if (d->comp[0].depth > maxDepth) return no("more bits per sample than the card decodes for this codec");
        }
    }
    return true;
}

// ---------------------------------------------------------------- devices and sessions
AVBufferRef* sharedDevice(AVHWDeviceType type)
{
    std::lock_guard<std::mutex> lock(gDeviceMutex);
    DeviceSlot& slot = gDevices[(int)type];
    if (!slot.ref && !slot.failed) {
        AVBufferRef* ref = nullptr;
        if (av_hwdevice_ctx_create(&ref, type, nullptr, nullptr, 0) >= 0 && ref) slot.ref = ref;
        else slot.failed = true;
    }
    return slot.ref ? av_buffer_ref(slot.ref) : nullptr;
}

std::shared_ptr<void> acquireSession()
{
    int n = gSessions.load();
    for (;;) {
        if (n >= gSessionLimit.load()) return nullptr;
        if (gSessions.compare_exchange_weak(n, n + 1)) break;
    }
    return std::shared_ptr<void>((void*)1, [](void*) { gSessions.fetch_sub(1); });
}

int activeSessions() { return gSessions.load(); }

namespace {
// What FFmpeg offers us once it has read the stream's headers.
AVPixelFormat pickFormat(AVCodecContext* c, const AVPixelFormat* list)
{
    State* st = static_cast<State*>(c->opaque);
    if (st)
        for (const AVPixelFormat* p = list; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == st->hwFormat) return *p;
    if (st) st->unsupported = true;
    for (const AVPixelFormat* p = list; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return list[0];
}
}  // namespace

bool attach(AVCodecContext* ctx, const AVCodec* codec, const AVCodecParameters* par, State& state, std::shared_ptr<void>& token)
{
    if (!ctx || !codec || !par || hardwareDecoding() == HwDecodeMode::Off) return false;
    if (!codecEligible(par->codec_id, par->profile, par->format, par->width, par->height)) return false;
    if (gEmulate.load()) {
        std::shared_ptr<void> session = acquireSession();
        if (!session) return false;
        state = State();
        state.emulated = true;
        state.name = "emulated";
        token = session;
        return true;
    }
    const std::vector<AVHWDeviceType> types = candidateTypes(hardwareDecoding(), hardwareDecodingApi(), currentPlatform(), availableTypes());
    if (types.empty()) return false;
    std::shared_ptr<void> session = acquireSession();
    if (!session) return false;                         // all the card's decoders are taken
    for (AVHWDeviceType t : types) {
        const AVCodecHWConfig* config = nullptr;
        for (int i = 0; const AVCodecHWConfig* c = avcodec_get_hw_config(codec, i); ++i)
            if ((c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && c->device_type == t) {
                config = c;
                break;
            }
        if (!config) continue;
        AVBufferRef* device = sharedDevice(t);
        if (!device) continue;
        state = State();
        state.type = t;
        state.hwFormat = config->pix_fmt;
        state.name = typeName(t);
        ctx->hw_device_ctx = device;
        ctx->get_format = pickFormat;
        ctx->opaque = &state;
        ctx->extra_hw_frames = 6;                       // surfaces for the frames the reader holds
        ctx->thread_count = 1;                          // the card does the work
        ctx->thread_type = FF_THREAD_SLICE;
        token = session;
        return true;
    }
    return false;
}

bool isOnCard(const State& state, const AVFrame* f)
{
    if (!f) return false;
    if (state.emulated) return f->opaque == (void*)&gMark;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)f->format);
    return d && (d->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

void markReceived(const State& state, AVFrame* f)
{
    if (state.emulated && f) f->opaque = (void*)&gMark;
}

namespace {
// A card hands frames over as NV12 (8 bits) or P010 (10 bits): the chroma samples interleaved in one plane.
bool repackToPlanar(ff::FramePtr& sw)
{
    const AVPixelFormat in = (AVPixelFormat)sw->format;
    if (in != AV_PIX_FMT_NV12 && in != AV_PIX_FMT_P010LE) return true;
    const bool deep = in == AV_PIX_FMT_P010LE;
    ff::FramePtr out(av_frame_alloc());
    if (!out) return false;
    out->format = deep ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    out->width = sw->width;
    out->height = sw->height;
    if (av_frame_get_buffer(out.get(), 32) < 0) return false;
    const int w = sw->width, h = sw->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int y = 0; y < h; ++y) {
        if (deep) {
            const uint16_t* s = (const uint16_t*)(sw->data[0] + (size_t)y * sw->linesize[0]);
            uint16_t* d = (uint16_t*)(out->data[0] + (size_t)y * out->linesize[0]);
            for (int x = 0; x < w; ++x) d[x] = (uint16_t)(s[x] >> 6);
        } else {
            std::memcpy(out->data[0] + (size_t)y * out->linesize[0], sw->data[0] + (size_t)y * sw->linesize[0], (size_t)w);
        }
    }
    for (int y = 0; y < ch; ++y) {
        if (deep) {
            const uint16_t* s = (const uint16_t*)(sw->data[1] + (size_t)y * sw->linesize[1]);
            uint16_t* u = (uint16_t*)(out->data[1] + (size_t)y * out->linesize[1]);
            uint16_t* v = (uint16_t*)(out->data[2] + (size_t)y * out->linesize[2]);
            for (int x = 0; x < cw; ++x) {
                u[x] = (uint16_t)(s[2 * x] >> 6);
                v[x] = (uint16_t)(s[2 * x + 1] >> 6);
            }
        } else {
            const uint8_t* s = sw->data[1] + (size_t)y * sw->linesize[1];
            uint8_t* u = out->data[1] + (size_t)y * out->linesize[1];
            uint8_t* v = out->data[2] + (size_t)y * out->linesize[2];
            for (int x = 0; x < cw; ++x) {
                u[x] = s[2 * x];
                v[x] = s[2 * x + 1];
            }
        }
    }
    if (av_frame_copy_props(out.get(), sw.get()) < 0) return false;
    sw = std::move(out);
    return true;
}
}  // namespace

bool toSystemMemory(const State& state, AVFrame* f)
{
    if (!isOnCard(state, f)) return true;
    ff::FramePtr sw(av_frame_alloc());
    if (!sw) return false;
    if (state.emulated) {
        const int n = ++gTransferAttempts;
        if (gFailTransferAfter.load() >= 0 && n > gFailTransferAfter.load()) return false;
        AVPixelFormat out = (AVPixelFormat)f->format;
        if (gEmulateSemiPlanar.load()) {
            if (out == AV_PIX_FMT_YUV420P) out = AV_PIX_FMT_NV12;
            else if (out == AV_PIX_FMT_YUV420P10LE) out = AV_PIX_FMT_P010LE;
        }
        sw->format = out;
        sw->width = f->width;
        sw->height = f->height;
        if (av_frame_get_buffer(sw.get(), 0) < 0) return false;
        if (out == f->format) {
            if (av_frame_copy(sw.get(), f) < 0) return false;
        } else {
            SwsContext* sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, f->width, f->height, out, SWS_POINT, nullptr, nullptr, nullptr);
            if (!sws) return false;
            const int n = sws_scale(sws, f->data, f->linesize, 0, f->height, sw->data, sw->linesize);
            sws_freeContext(sws);
            if (n <= 0) return false;
        }
    } else {
        if (av_hwframe_transfer_data(sw.get(), f, 0) < 0) return false;
    }
    if (av_frame_copy_props(sw.get(), f) < 0 || !repackToPlanar(sw)) return false;
    sw->opaque = nullptr;
    av_frame_unref(f);
    av_frame_move_ref(f, sw.get());
    ++gTransfers;
    return true;
}

void setEmulation(bool on, int failTransferAfter, int failDecodeAfter, bool semiPlanar)
{
    gEmulate.store(on);
    gEmulateSemiPlanar.store(semiPlanar);
    gFailTransferAfter.store(failTransferAfter);
    gFailDecodeAfter.store(failDecodeAfter);
    gTransferAttempts.store(0);
    gDecodedEmulated.store(0);
}

bool emulationFailsDecode()
{
    if (!gEmulate.load() || gFailDecodeAfter.load() < 0) return false;
    if (++gDecodedEmulated <= gFailDecodeAfter.load()) return false;
    gFailDecodeAfter.store(-1);
    return true;
}

long transfers() { return gTransfers.load(); }
void resetCounters()
{
    gTransfers.store(0);
    gTransferAttempts.store(0);
}

}  // namespace hw
}  // namespace reno
