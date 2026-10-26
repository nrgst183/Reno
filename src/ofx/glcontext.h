// OpenGL contexts for rendering effects on the graphics card (OpenFX's OpenGL render suite), made without a visible window: WGL with a hidden window on Windows, EGL with a pbuffer
// (surfaceless when the driver can) elsewhere.
//
// The OpenGL functions the host needs are loaded by hand (no GLEW); effects load their own. Contexts are kept in a small pool: a render takes one (Lease), has it current on its thread
// while it holds the lease, and gives it back. A thread gets the context it used last when that is free, so what an effect set up in it (kOfxActionOpenGLContextAttached) is reused.
// Nothing here is fatal: no context, no frame buffers or a failing driver just make available() false and effects render on the processor.
#pragma once
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
class Instance;
namespace gl {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;

enum : GLenum {
    TEXTURE_2D = 0x0DE1,
    TEXTURE_MIN_FILTER = 0x2801,
    TEXTURE_MAG_FILTER = 0x2800,
    TEXTURE_WRAP_S = 0x2802,
    TEXTURE_WRAP_T = 0x2803,
    LINEAR = 0x2601,
    CLAMP_TO_EDGE = 0x812F,
    RGBA = 0x1908,
    BGRA = 0x80E1,
    RGBA8 = 0x8058,
    RGBA32F = 0x8814,
    UNSIGNED_BYTE = 0x1401,
    FLOAT = 0x1406,
    UNPACK_ALIGNMENT = 0x0CF5,
    UNPACK_ROW_LENGTH = 0x0CF2,
    PACK_ALIGNMENT = 0x0D05,
    PACK_ROW_LENGTH = 0x0D02,
    FRAMEBUFFER = 0x8D40,
    RENDERBUFFER = 0x8D41,
    COLOR_ATTACHMENT0 = 0x8CE0,
    DEPTH_STENCIL_ATTACHMENT = 0x821A,
    DEPTH24_STENCIL8 = 0x88F0,
    FRAMEBUFFER_COMPLETE = 0x8CD5,
    COLOR_BUFFER_BIT = 0x4000,
    TEXTURE0 = 0x84C0,
    NO_ERROR_ = 0,
    FRAMEBUFFER_BINDING = 0x8CA6,
    VERSION = 0x1F02,
    RENDERER = 0x1F01,
    MAX_TEXTURE_SIZE = 0x0D33,
    PIXEL_PACK_BUFFER = 0x88EB,
    PIXEL_UNPACK_BUFFER = 0x88EC,
};

// The functions the host calls (all of them core OpenGL 3.0 or the EXT/ARB framebuffer object ones).
struct Funcs {
    GLenum (*GetError)() = nullptr;
    void (*Finish)() = nullptr;
    void (*GenTextures)(GLsizei, GLuint*) = nullptr;
    void (*DeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void (*BindTexture)(GLenum, GLuint) = nullptr;
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void (*TexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void (*PixelStorei)(GLenum, GLint) = nullptr;
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*Clear)(GLbitfield) = nullptr;
    void (*GetIntegerv)(GLenum, GLint*) = nullptr;
    const unsigned char* (*GetString)(GLenum) = nullptr;
    void (*ActiveTexture)(GLenum) = nullptr;
    void (*BindBuffer)(GLenum, GLuint) = nullptr;
    void (*UseProgram)(GLuint) = nullptr;
    void (*ReadBuffer)(GLenum) = nullptr;
    void (*DrawBuffer)(GLenum) = nullptr;
    void (*GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void (*DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum (*CheckFramebufferStatus)(GLenum) = nullptr;
    void (*GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void (*DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void (*BindRenderbuffer)(GLenum, GLuint) = nullptr;
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
};

class Context {
public:
    ~Context();
    int id = 0;
    Funcs fn;
    std::string renderer;
    // The effects attached to this context (kOfxActionOpenGLContextAttached sent, Detached not yet); guarded by 'mutex'.
    std::mutex mutex;
    std::set<Instance*> attached;

    struct Impl;
    Impl* impl = nullptr;
};

// Whether a context can be made at all here (tried once, then remembered; a context that was made is kept for use).
bool available();
// Why not, when it is not (for the log).
std::string unavailableReason();

// Exclusive use of one context, current on this thread, until the lease is let go or destroyed.
class Lease {
public:
    Lease() {}
    Lease(Lease&& o) noexcept : ctx_(o.ctx_), prev_(o.prev_) { o.ctx_ = nullptr; o.prev_ = nullptr; }
    Lease& operator=(Lease&& o) noexcept;
    ~Lease() { release(); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    // The context this thread used last when it is free, else any free one (a new one while fewer than the limit exist, else waits). 'want': exactly that context (waits until free).
    // Check ok(). A thread that holds a lease and takes another (an effect's frame asked for while it renders makes the effects before it render) gets a context of its own even beyond
    // the limit, and has the first one current again when the second is let go.
    static Lease acquire(Context* want = nullptr);
    bool ok() const { return ctx_ != nullptr; }
    Context* ctx() const { return ctx_; }
    Context* operator->() const { return ctx_; }
    void release();

private:
    Context* ctx_ = nullptr;
    Context* prev_ = nullptr;           // the lease this thread held when this one was taken
};

// The contexts that exist (for tests, and for detaching at exit).
int contextCount();
// Sends every effect still attached the Detached action (each in its context), then destroys all contexts. Called when
// the program is done with the graphics card; contexts made afterwards start again from nothing.
void shutdown();

// Set by the host (gpurender.cpp): what to do for an effect that must be detached from a context (the context is current).
extern void (*detachHook)(Instance* inst);

}  // namespace gl
}  // namespace ofx
}  // namespace reno
