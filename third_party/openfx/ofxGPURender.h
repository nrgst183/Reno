#ifndef _ofxGPURender_h_
#define _ofxGPURender_h_

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause
//
// The OpenGL part of OpenFX 1.4's GPU rendering header (the OpenGL render suite, which VEGAS Pro and NewBlue's effects
// use), written out from the specification's names. The CUDA, Metal and OpenCL parts are not here.

#include "ofxCore.h"
#include "ofxImageEffect.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Name of the OpenGL render suite. */
#define kOfxOpenGLRenderSuite "OfxImageEffectOpenGLRenderSuite"

/** @brief Whether a host or a plug-in can render with OpenGL.
    On the host: "true" or "false". On a plug-in (its descriptor, or an instance): "false", "true" (it can, and the host
    may choose) or "needed" (it cannot render without OpenGL). The default is "false". */
#define kOfxImageEffectPropOpenGLRenderSupported "OfxImageEffectPropOpenGLRenderSupported"

/** @brief In the render actions (and Begin/EndSequenceRender): 1 if this render is to be done with OpenGL, in the
    context that is current on the calling thread, 0 if on the processor. */
#define kOfxImageEffectPropOpenGLEnabled "OfxImageEffectPropOpenGLEnabled"

/** @brief On a texture handed out by clipLoadTexture: the OpenGL name of the texture (an int). */
#define kOfxImageEffectPropOpenGLTextureIndex "OfxImageEffectPropOpenGLTextureIndex"

/** @brief On a texture handed out by clipLoadTexture: its OpenGL target (GL_TEXTURE_2D, an int). */
#define kOfxImageEffectPropOpenGLTextureTarget "OfxImageEffectPropOpenGLTextureTarget"

/** @brief Sent to an instance when an OpenGL context it will render in is current for the first time. Resources it
    creates (programs, textures) live in that context until kOfxActionOpenGLContextDetached. */
#define kOfxActionOpenGLContextAttached "OfxActionOpenGLContextAttached"

/** @brief Sent to an instance with the context it was attached to current, before the context or the instance goes. */
#define kOfxActionOpenGLContextDetached "OfxActionOpenGLContextDetached"

/** @brief The OpenGL render suite. */
typedef struct OfxImageEffectOpenGLRenderSuiteV1 {
    /** Makes a texture of the clip's image at 'time' in the current context. 'format' (may be NULL: the clip's depth)
        names the depth wanted (kOfxBitDepthByte: RGBA8, kOfxBitDepthFloat: RGBA32F); 'region' (may be NULL: all of it) is in
        canonical coordinates. The handle carries the usual image properties (bounds, region of definition, render scale...)
        and kOfxImageEffectPropOpenGLTextureIndex / TextureTarget. For the output clip it is the texture the host has
        attached to the frame buffer it bound for the render. */
    OfxStatus (*clipLoadTexture)(OfxImageClipHandle clip, OfxTime time, const char* format, const OfxRectD* region,
                                 OfxPropertySetHandle* textureHandle);
    /** Lets go of a texture from clipLoadTexture. */
    OfxStatus (*clipFreeTexture)(OfxPropertySetHandle textureHandle);
    /** Asks the host to free what it keeps for rendering with OpenGL (textures it cached). */
    OfxStatus (*flushResources)();
} OfxImageEffectOpenGLRenderSuiteV1;

#ifdef __cplusplus
}
#endif

#endif
