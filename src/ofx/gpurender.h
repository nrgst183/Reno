// Rendering OpenFX effects with OpenGL (OpenFX's OpenGL render suite, kOfxOpenGLRenderSuite, as VEGAS Pro has it and NewBlue's effects use it): the host makes a context current on the render
// thread, hands the effect its source frame as a texture (clipLoadTexture) and a frame buffer to draw into, and reads the result back. Any failure renders that frame on the processor
// (Instance::render), and an effect whose OpenGL render fails is not asked to use OpenGL again.
//
// Frames are bottom-left origin and premultiplied, as everywhere in Reno. 8-bit frames (blue-first in memory) are uploaded as RGBA8 textures through GL_BGRA, so the effect sees red, green,
// blue, alpha; float ones as RGBA32F.
#pragma once
#include "ofxhost.h"
#include "ofxGPURender.h"
#include "glcontext.h"
#include <memory>
#include <thread>

namespace reno {
namespace ofx {

// Whether this effect is to be rendered with OpenGL now: the host is set to use the graphics card (HostConfig::gpu) or
// the effect needs it, a context can be made, the effect says it can (kOfxImageEffectPropOpenGLRenderSupported "true"
// or "needed") and has not failed with OpenGL before.
bool gpuWanted(const Instance& inst);

// One render of one effect with OpenGL: holds a context (current on this thread), the frame buffer the effect draws
// into and the textures it loaded, until it is destroyed. Make it, check ready(), run the effect's render actions with
// kOfxImageEffectPropOpenGLEnabled 1, then readBack().
class GpuRender {
public:
    GpuRender(Instance& inst, int width, int height);
    ~GpuRender();
    GpuRender(const GpuRender&) = delete;
    GpuRender& operator=(const GpuRender&) = delete;
    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }
    bool readBack(Image& out);

    OfxStatus loadTexture(ClipInst* clip, double time, const char* format, OfxPropertySetHandle* handle);
    OfxStatus freeTexture(OfxPropertySetHandle handle);
    bool onRenderThread() const;
    GpuRender* outer() const { return outer_; }

private:
    struct Tex;
    Instance& inst_;
    gl::Lease lease_;
    int w_, h_;
    PixelDepth depth_;
    bool ready_ = false;
    std::string error_;
    unsigned fbo_ = 0, outTex_ = 0, depthRb_ = 0;
    std::vector<std::unique_ptr<Tex>> texes_;
    std::mutex mutex_;
    std::thread::id thread_;
    GpuRender* outer_ = nullptr;        // the GL render this thread was in when this one began
    bool hostError(const char* what);
};

// The effect is being destroyed: sends it kOfxActionOpenGLContextDetached in each context it was attached to.
void gpuInstanceGone(Instance& inst);
// The OpenGL render suite (null if no context can be made).
const OfxImageEffectOpenGLRenderSuiteV1* gpuSuite();

}  // namespace ofx
}  // namespace reno
