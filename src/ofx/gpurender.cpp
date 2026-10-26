#include "gpurender.h"
#include "ofxinternal.h"
#include <algorithm>
#include <cstring>

namespace reno {
namespace ofx {

using namespace gl;

// ====================================================================== which effects, which contexts
bool gpuWanted(const Instance& inst)
{
    const Plugin* pl = inst.plugin;
    if (!pl || pl->glFailed.load()) return false;
    const std::string sup = inst.props.getString(kOfxImageEffectPropOpenGLRenderSupported, 0, "false");
    if (sup != "true" && sup != "needed") return false;
    if (!Host::get().config.gpu && sup != "needed") return false;
    return available();
}

namespace {
thread_local GpuRender* tCurrent = nullptr;       // the GL render this thread is in (the innermost)
// The contexts each effect is attached to (guarded by gAttachMutex); the other side is Context::attached.
std::mutex gAttachMutex;
std::map<Instance*, std::set<Context*>> gAttached;

void callDetach(Instance* inst)
{
    // the context is current on this thread
    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
    if (inst->plugin && !inst->plugin->crashed) inst->plugin->call(kOfxActionOpenGLContextDetached, inst, nullptr, nullptr);
}

void detachFromHook(Instance* inst)
{
    {
        std::lock_guard<std::mutex> lk(gAttachMutex);
        auto it = gAttached.find(inst);
        if (it != gAttached.end()) {
            gAttached.erase(it);
        }
    }
    callDetach(inst);
}

struct HookInstaller {
    HookInstaller() { gl::detachHook = detachFromHook; }
} gHookInstaller;
}  // namespace

void gpuInstanceGone(Instance& inst)
{
    std::set<Context*> where;
    {
        std::lock_guard<std::mutex> lk(gAttachMutex);
        auto it = gAttached.find(&inst);
        if (it == gAttached.end()) return;
        where = it->second;
        gAttached.erase(it);
    }
    for (Context* c : where) {
        Lease l = Lease::acquire(c);
        if (!l.ok()) continue;
        bool was = false;
        {
            std::lock_guard<std::mutex> lk(c->mutex);
            was = c->attached.erase(&inst) > 0;
        }
        if (was) callDetach(&inst);
    }
}

// ====================================================================== textures
struct GpuRender::Tex {
    PropertySet props;
    GLuint name = 0;
    bool output = false;        // the frame buffer's own texture: not freed with the handle
    bool freed = false;
};

static const char* depthName(PixelDepth d) { return d == PixelDepth::Float ? kOfxBitDepthFloat : kOfxBitDepthByte; }

GpuRender::GpuRender(Instance& inst, int width, int height) : inst_(inst), w_(width), h_(height), depth_(inst.depth()), thread_(std::this_thread::get_id())
{
    lease_ = Lease::acquire();
    outer_ = tCurrent;
    tCurrent = this;
    if (!lease_.ok()) {
        error_ = "no OpenGL context";
        return;
    }
    Context* ctx = lease_.ctx();
    Funcs& f = ctx->fn;
    while (f.GetError() != NO_ERROR_) {}
    bool attach = false;
    {
        std::lock_guard<std::mutex> lk(ctx->mutex);
        attach = ctx->attached.insert(&inst_).second;
    }
    if (attach) {
        {
            std::lock_guard<std::mutex> lk(gAttachMutex);
            gAttached[&inst_].insert(ctx);
        }
        const OfxStatus st = inst_.plugin->call(kOfxActionOpenGLContextAttached, &inst_, nullptr, nullptr);
        if (inst_.plugin->crashed) {
            error_ = "crashed when attached to the OpenGL context";
            return;
        }
        if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
            error_ = strfmt("would not attach to the OpenGL context (%s)", inst_.plugin->explainStatus(st).c_str());
            return;
        }
    }
    if (w_ <= 0 || h_ <= 0) {
        error_ = "empty picture";
        return;
    }
    const bool flt = depth_ == PixelDepth::Float;
    f.GenTextures(1, &outTex_);
    f.BindTexture(TEXTURE_2D, outTex_);
    f.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    f.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    f.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
    f.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
    f.TexImage2D(TEXTURE_2D, 0, flt ? RGBA32F : RGBA8, w_, h_, 0, RGBA, flt ? FLOAT : UNSIGNED_BYTE, nullptr);
    f.BindTexture(TEXTURE_2D, 0);
    f.GenFramebuffers(1, &fbo_);
    f.BindFramebuffer(FRAMEBUFFER, fbo_);
    f.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, outTex_, 0);
    f.GenRenderbuffers(1, &depthRb_);
    f.BindRenderbuffer(RENDERBUFFER, depthRb_);
    f.RenderbufferStorage(RENDERBUFFER, DEPTH24_STENCIL8, w_, h_);
    f.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_STENCIL_ATTACHMENT, RENDERBUFFER, depthRb_);
    f.BindRenderbuffer(RENDERBUFFER, 0);
    if (f.CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) {
        // no depth / stencil: most effects do not need them
        f.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_STENCIL_ATTACHMENT, RENDERBUFFER, 0);
        f.DeleteRenderbuffers(1, &depthRb_);
        depthRb_ = 0;
    }
    if (f.CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) {
        error_ = "the frame buffer for the effect is not complete";
        return;
    }
    f.DrawBuffer(COLOR_ATTACHMENT0);
    f.Viewport(0, 0, w_, h_);
    f.ClearColor(0.f, 0.f, 0.f, 0.f);
    f.Clear(COLOR_BUFFER_BIT);
    if (hostError("making the frame buffer")) return;
    ready_ = true;
    inst_.gpuFrame.store(this);
}

GpuRender::~GpuRender()
{
    tCurrent = outer_;
    inst_.gpuFrame.store(nullptr);
    if (!lease_.ok()) return;
    Funcs& f = lease_.ctx()->fn;
    for (auto& t : texes_)
        if (!t->freed && !t->output && t->name) f.DeleteTextures(1, &t->name);
    texes_.clear();
    f.BindFramebuffer(FRAMEBUFFER, 0);
    f.BindTexture(TEXTURE_2D, 0);
    if (fbo_) f.DeleteFramebuffers(1, &fbo_);
    if (depthRb_) f.DeleteRenderbuffers(1, &depthRb_);
    if (outTex_) f.DeleteTextures(1, &outTex_);
    while (f.GetError() != NO_ERROR_) {}
}

bool GpuRender::onRenderThread() const { return std::this_thread::get_id() == thread_; }

bool GpuRender::hostError(const char* what)
{
    if (!lease_.ok()) return true;
    const GLenum e = lease_.ctx()->fn.GetError();
    if (e == NO_ERROR_) return false;
    while (lease_.ctx()->fn.GetError() != NO_ERROR_) {}
    error_ = strfmt("OpenGL error 0x%x while %s", (unsigned)e, what);
    return true;
}

static void fillTextureProps(PropertySet& s, int w, int h, PixelDepth depth, GLuint name, const double scale[2], const std::string& id)
{
    s.setString(kOfxPropType, kOfxTypeImage);
    s.setString(kOfxImageEffectPropPixelDepth, depthName(depth));
    s.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    s.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    s.setDouble(kOfxImageEffectPropRenderScale, scale[0], 0);
    s.setDouble(kOfxImageEffectPropRenderScale, scale[1], 1);
    s.setDouble(kOfxImagePropPixelAspectRatio, 1.0);
    const int b[4] = {0, 0, w, h};
    for (int k = 0; k < 4; ++k) {
        s.setInt(kOfxImagePropBounds, b[k], k);
        s.setInt(kOfxImagePropRegionOfDefinition, b[k], k);
    }
    s.setString(kOfxImagePropField, kOfxImageFieldNone);
    s.setString(kOfxImagePropUniqueIdentifier, id);
    s.setInt(kOfxImageEffectPropOpenGLTextureIndex, (int)name);
    s.setInt(kOfxImageEffectPropOpenGLTextureTarget, (int)TEXTURE_2D);
}

OfxStatus GpuRender::loadTexture(ClipInst* c, double time, const char* format, OfxPropertySetHandle* handle)
{
    if (!c || !handle) return kOfxStatErrBadHandle;
    *handle = nullptr;
    if (!lease_.ok() || !ready_ || !onRenderThread()) return kOfxStatErrBadHandle;
    Funcs& f = lease_.ctx()->fn;
    std::lock_guard<std::mutex> lk(mutex_);
    std::unique_ptr<Tex> t(new Tex());
    t->props.owner = t.get();
    const double scale[2] = {inst_.renderScale[0], inst_.renderScale[1]};
    if (c->isOutput) {
        t->name = outTex_;
        t->output = true;
        fillTextureProps(t->props, w_, h_, depth_, outTex_, scale, strfmt("%s#gl%u", c->name.c_str(), outTex_));
        f.BindFramebuffer(FRAMEBUFFER, fbo_);
        f.Viewport(0, 0, w_, h_);
    } else {
        ImagePtr img = c->source ? c->source(time) : ImagePtr();
        if (!img) return kOfxStatFailed;
        // the kind of texture asked for (the clip's own when not said)
        PixelDepth want = depth_;
        if (format) {
            const std::string fm = format;
            if (fm == kOfxBitDepthFloat || fm.find("32F") != std::string::npos) want = PixelDepth::Float;
            else if (fm == kOfxBitDepthByte || fm.find("RGBA8") != std::string::npos) want = PixelDepth::Byte;
        }
        if (img->depth != want) img = img->converted(want);
        if (!img) return kOfxStatErrMemory;
        const bool flt = img->depth == PixelDepth::Float;
        const size_t bpp = img->pixelBytes();
        std::vector<uint8_t> packed;
        const void* data = img->data.get();
        GLint rowLength = 0;
        if (img->rowBytes % bpp == 0) rowLength = (GLint)(img->rowBytes / bpp);
        else {
            packed.resize((size_t)img->w * bpp * (size_t)img->h);
            for (int y = 0; y < img->h; ++y) std::memcpy(packed.data() + (size_t)y * img->w * bpp, img->row(y), (size_t)img->w * bpp);
            data = packed.data();
            rowLength = img->w;
        }
        GLuint name = 0;
        f.GenTextures(1, &name);
        f.BindTexture(TEXTURE_2D, name);
        f.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
        f.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
        f.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        f.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        f.BindBuffer(PIXEL_UNPACK_BUFFER, 0);
        f.PixelStorei(UNPACK_ALIGNMENT, 1);
        f.PixelStorei(UNPACK_ROW_LENGTH, rowLength);
        f.TexImage2D(TEXTURE_2D, 0, flt ? RGBA32F : RGBA8, img->w, img->h, 0, flt ? RGBA : BGRA, flt ? FLOAT : UNSIGNED_BYTE, data);
        f.PixelStorei(UNPACK_ROW_LENGTH, 0);
        f.BindTexture(TEXTURE_2D, 0);
        t->name = name;
        if (hostError("loading a texture")) {
            f.DeleteTextures(1, &name);
            return kOfxStatErrMemory;
        }
        fillTextureProps(t->props, img->w, img->h, img->depth, name, scale, strfmt("%s@%.4f#gl%u", c->name.c_str(), time, name));
    }
    *handle = (OfxPropertySetHandle)&t->props;
    texes_.push_back(std::move(t));
    return kOfxStatOK;
}

OfxStatus GpuRender::freeTexture(OfxPropertySetHandle handle)
{
    std::lock_guard<std::mutex> lk(mutex_);
    PropertySet* s = asProps(handle);
    if (!s) return kOfxStatErrBadHandle;
    for (auto& t : texes_) {
        if (&t->props != s) continue;
        if (t->freed) return kOfxStatErrBadHandle;
        t->freed = true;
        if (!t->output && t->name && lease_.ok() && onRenderThread()) {
            lease_.ctx()->fn.DeleteTextures(1, &t->name);
            t->name = 0;
        }
        return kOfxStatOK;
    }
    return kOfxStatErrBadHandle;
}

bool GpuRender::readBack(Image& out)
{
    if (!lease_.ok() || !ready_) return false;
    if (out.w != w_ || out.h != h_ || out.depth != depth_) {
        error_ = "the picture is not the size of the frame buffer";
        return false;
    }
    Funcs& f = lease_.ctx()->fn;
    f.Finish();
    f.BindFramebuffer(FRAMEBUFFER, fbo_);
    while (f.GetError() != NO_ERROR_) {}
    const bool flt = depth_ == PixelDepth::Float;
    const size_t bpp = out.pixelBytes();
    f.BindBuffer(PIXEL_PACK_BUFFER, 0);
    f.UseProgram(0);
    f.ReadBuffer(COLOR_ATTACHMENT0);
    f.PixelStorei(PACK_ALIGNMENT, 1);
    f.PixelStorei(PACK_ROW_LENGTH, 0);
    if (out.rowBytes == (size_t)w_ * bpp) {
        f.ReadPixels(0, 0, w_, h_, flt ? RGBA : BGRA, flt ? FLOAT : UNSIGNED_BYTE, out.data.get());
    } else {
        for (int y = 0; y < h_; ++y) f.ReadPixels(0, y, w_, 1, flt ? RGBA : BGRA, flt ? FLOAT : UNSIGNED_BYTE, out.row(y));
    }
    f.BindFramebuffer(FRAMEBUFFER, 0);
    return !hostError("reading the picture back");
}

// ====================================================================== the suite
static GpuRender* frameOf(Instance* inst) { return inst ? static_cast<GpuRender*>(inst->gpuFrame.load()) : nullptr; }

static OfxStatus glClipLoadTexture(OfxImageClipHandle h, OfxTime time, const char* format, const OfxRectD*, OfxPropertySetHandle* texture)
{
    if (texture) *texture = nullptr;
    ClipInst* c = asClip(h);
    if (!c || !texture) return kOfxStatErrBadHandle;
    GpuRender* g = frameOf(c->inst);
    if (!g) return kOfxStatErrBadHandle;        // not in a render done with OpenGL
    return g->loadTexture(c, time, format, texture);
}

static OfxStatus glClipFreeTexture(OfxPropertySetHandle texture)
{
    PropertySet* s = asProps(texture);
    if (!s || !s->owner) return kOfxStatErrBadHandle;
    // (a texture is only ever freed on the thread that is rendering with it: the innermost GL render of this thread, or
    // one outside it when effects rendering for it are nested)
    for (GpuRender* g = tCurrent; g; g = g->outer())
        if (g->freeTexture(texture) == kOfxStatOK) return kOfxStatOK;
    return kOfxStatErrBadHandle;
}

static OfxStatus glFlushResources() { return kOfxStatOK; }

static const OfxImageEffectOpenGLRenderSuiteV1 gGlSuite = {glClipLoadTexture, glClipFreeTexture, glFlushResources};

const OfxImageEffectOpenGLRenderSuiteV1* gpuSuite() { return available() ? &gGlSuite : nullptr; }

}  // namespace ofx
}  // namespace reno
