// VEGAS Pro's additions to OpenFX, as far as a host needs them to run effects written for VEGAS. Written from the names the installed effects
// ask for and the published description of the extension (property/action names are the strings a plug-in passes; suite layouts follow the order
// of the functions a plug-in calls). Effects that stick to standard OpenFX need none of it.
#pragma once
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxParam.h"

// ---- host names VEGAS reports (effects check these to switch their VEGAS behaviour on)
#define kRenoVegasHostName "com.vegascreativesoftware.vegas"          // VEGAS Pro 14 and later
#define kRenoSonyVegasHostName "com.sonycreativesoftware.vegas"        // VEGAS Pro 13 and older (Sony)

// ---- host / instance properties
#define kVegasPropHostAppDataDirectory "OfxPropVegasHostAppDataDirectory"   // string, host
#define kVegasPropHostHWnd             "OfxPropVegasHostHWnd"               // pointer, host (main window)
// A string every effect built on VEGAS's copy of the OpenFX support library reads from the host when it loads (it fails with kOfxStatErrMissingHostFeature
// if absent). VEGAS fills it from a text of its own (vegas180.exe: CSvfxPropertySetHost keeps it at +0x20); effects only copy it (read in the support
// library's fetchHostDescription and dropped). Reno gives an empty one.
#define kVegasPropSpikeKey             "OfxPropVegasSpikeKey"               // string, host
// A COM object of the host (IUnknown) that Titles & Text queries ({195E13F0-C2A2-4158-964A-685822244C9A}, then its fourth method with the assembly
// "ScriptPortal.MediaSoftware.TextGen.OFXInterop" and type "...OFXInterop.FilterFactory3") to make the .NET object doing its work. Reno has no such
// factory: Titles & Text reports that when made, and Reno's built-in Titles & Text is used instead.
#define kVegasPropHostIUnknown         "OfxPropHostIUnknown"                // pointer, host

// ---- effect descriptor: GUIDs of the older (DirectX-era) VEGAS plug-ins an effect replaces. The host gives the property with no values; each effect
// appends its own while describing itself in a context (VEGAS's descriptor answers the dimension with the count so far).
#define kVegasPropUpliftGUID           "OfxImageEffectPropVegasUpliftGUID"  // string x N, effect descriptor
// ---- IsIdentity and Render arguments: the timeline time (project frames, not rounded), next to kOfxPropTime, the effect's own time. Timecode and others
// fail with kOfxStatErrMissingHostFeature if it is missing.
#define kVegasPropTimeCode             "OfxPropVegasTimeCode"               // double, render / isIdentity arguments
#define kVegasPropContext              "OfxImageEffectPropVegasContext"     // string, effect instance
#define kVegasContextUnknown           "OfxImageEffectPropVegasContextUnknown"
#define kVegasContextMedia             "OfxImageEffectPropVegasContextMedia"
#define kVegasContextTrack             "OfxImageEffectPropVegasContextTrack"
#define kVegasContextEvent             "OfxImageEffectPropVegasContextEvent"
#define kVegasContextEventFadeIn       "OfxImageEffectPropVegasContextEventFadeIn"
#define kVegasContextEventFadeOut      "OfxImageEffectPropVegasContextEventFadeOut"
#define kVegasContextProject           "OfxImageEffectPropVegasContextProject"
#define kVegasContextGenerator         "OfxImageEffectPropVegasContextGenerator"

// ---- frames: 8-bit frames are handed over blue first
#define kVegasPropPixelOrder  "OfxImageEffectPropPixelOrder"                // string, clip and image
#define kVegasPixelOrderRGBA  "OfxImagePixelOrderRGBA"
#define kVegasPixelOrderBGRA  "OfxImagePixelOrderBGRA"

// ---- render arguments
#define kVegasPropViewsToRender "OfxImageEffectPropViewsToRender"           // int: 1 = ordinary, 2 = stereoscopic
#define kVegasPropRenderView    "OfxImageEffectPropRenderView"              // int: 0 = ordinary or left, 1 = right
#define kVegasPropRenderQuality "OfxImageEffectPropRenderQuality"           // string
#define kVegasRenderQualityDraft   "OfxImageEffectPropRenderQualityDraft"
#define kVegasRenderQualityPreview "OfxImageEffectPropRenderQualityPreview"
#define kVegasRenderQualityGood    "OfxImageEffectPropRenderQualityGood"
#define kVegasRenderQualityBest    "OfxImageEffectPropRenderQualityBest"

// ---- About / Help
#define kVegasActionInvokeAbout "OfxImageEffectActionInvokeAbout"
#define kVegasActionInvokeHelp  "OfxImageEffectActionInvokeHelp"

// ---- a window of the effect's own at the top of its panel ("HWnd interact")
#define kVegasHWndInteractSuite        "OfxHWndInteractSuite"
#define kVegasPluginPropHWndInteractV1 "OfxImageEffectPluginPropHWndInteractV1"   // pointer, effect descriptor: an OfxPluginEntryPoint
#define kVegasHWndPropParent           "OfxHWndInteractPropParent"                // pointer (HWND), in: create
#define kVegasHWndPropMinSize          "OfxHWndInteractPropMinSize"               // int x 2, out: create
#define kVegasHWndPropPreferredSize    "OfxHWndInteractPropPrefferedSize"         // int x 2, out: create (spelt this way)
#define kVegasHWndPropLocation         "OfxHWndInteractPropLocation"              // int x 4 (left, top, width, height), in: move
#define kVegasHWndActionCreateWindow   "OfxHWndInteractActionCreateWindow"
#define kVegasHWndActionMoveWindow     "OfxHWndInteractActionMoveWindow"
#define kVegasHWndActionDisposeWindow  "OfxHWndInteractActionDisposeWindow"
#define kVegasHWndActionShowWindow     "OfxHWndInteractActionShowWindow"

typedef struct RenoVegasHWndInteractSuiteV1 {
    OfxStatus (*interactGetPropertySet)(OfxInteractHandle interactInstance, OfxPropertySetHandle* property);
    OfxStatus (*interactUpdate)(OfxInteractHandle interactInstance);
} RenoVegasHWndInteractSuiteV1;

// ---- progress
#define kVegasProgressSuite "OfxVegasProgressSuite"
typedef struct RenoVegasProgressSuiteV1 {
    OfxStatus (*progressStart)(void* effectInstance, const char* message, const char* messageId);
    OfxStatus (*progressUpdate)(void* effectInstance, double progress);
    OfxStatus (*progressEnd)(void* effectInstance);
} RenoVegasProgressSuiteV1;
typedef struct RenoVegasProgressSuiteV2 {
    OfxStatus (*progressStart)(void* effectInstance, const char* message, const char* messageId, int showTimeWindows);
    OfxStatus (*progressUpdate)(void* effectInstance, double progress);
    OfxStatus (*progressEnd)(void* effectInstance);
} RenoVegasProgressSuiteV2;

// ---- stereoscopic frames
#define kVegasStereoscopicSuite "OfxVegasStereoscopicImageEffectSuite"
typedef struct RenoVegasStereoscopicSuiteV1 {
    OfxStatus (*clipGetStereoscopicImage)(OfxImageClipHandle clip, OfxTime time, int view, OfxRectD* region, OfxPropertySetHandle* imageHandle);
} RenoVegasStereoscopicSuiteV1;

// ---- keyframe shapes
#define kVegasKeyframeSuite "OfxVegasKeyframeSuite"
#define kVegasKeyInterpUnknown "OfxVegasKeyframeInterpolationUnknown"
#define kVegasKeyInterpLinear  "OfxVegasKeyframeInterpolationLinear"
#define kVegasKeyInterpHold    "OfxVegasKeyframeInterpolationHold"
typedef struct RenoVegasKeyframeSuiteV1 {
    OfxStatus (*paramGetKeyInterpolation)(OfxParamHandle param, OfxTime time, char** interpolationType);
    OfxStatus (*paramGetKeySlopes)(OfxParamHandle param, OfxTime time, ...);
    OfxStatus (*paramSetKeyInterpolation)(OfxParamHandle param, OfxTime time, const char* interpolationType);
    OfxStatus (*paramSetKeySlopes)(OfxParamHandle param, OfxTime time, ...);
} RenoVegasKeyframeSuiteV1;

// ---- VEGAS's parameter holding a frame grabbed from the preview (Color Match). VEGAS keeps the two properties below on it; its parameter suite has no
// value for it (kOfxStatErrUnsupported).
#define kVegasParamTypeImage                "OfxParamTypeImage"
#define kVegasParamImageHideGrabFromPreview "OfxParamImageHideGrabFromPreview"     // int, parameter
#define kVegasParamImageHideGrabControls    "OfxParamImageHideGrabControls"        // int, parameter

// ---- About / Help asked of the host by the effect
// VEGAS Pro 18's table (vegas180.exe) has one function. An effect calls it with its own handle and one of the two action names below to have the host
// show its Help or About; VEGAS answers kOfxStatOK for those two and kOfxStatFailed for a null handle or any other name. Its stock effects call it
// without checking that the host gave the suite (a host without it crashes them on About or Help).
#define kVegasEffectSuite              "OfxVegasEffectSuite"
#define kVegasActionShowEffectHelp     "OfxVegasActionShowEffectHelp"
#define kVegasActionShowEffectAbout    "OfxVegasActionShowEffectAbout"
typedef struct RenoVegasEffectSuiteV1 {
    OfxStatus (*effectAction)(OfxImageEffectHandle effect, const char* action);
} RenoVegasEffectSuiteV1;

// ---- on-screen controls drawn with Windows GDI over the preview ("HWnd overlay"): VEGAS's table has two functions, the overlay's property set (made on
// first request) and "draw me again". Reno gives the suite but does not run such overlays: the actions (OfxHWndOverlayInteractDraw, ...MouseDown, ...)
// are never sent.
#define kVegasHWndOverlayInteractSuite "OfxHWndOverlayInteractSuite"
#define kVegasPluginPropHWndOverlayInteractV1 "OfxImageEffectPluginPropHWndOverlayInteractV1"
typedef struct RenoVegasHWndOverlaySuiteV1 {
    OfxStatus (*getPropertySet)(void* overlayHandle, OfxPropertySetHandle* property);
    OfxStatus (*redraw)(void* overlayHandle);
} RenoVegasHWndOverlaySuiteV1;

// ---- a suite Reno does not give: VEGAS compiles OpenCL programs for effects that render on the graphics card (one function). Effects check for it and
// render on the processor when it is missing (reporting kOfxImageEffectPropOpenCLSupported "false"), which is what Reno wants.
#define kVegasOpenCLProgramSuite       "OfxOpenCLProgramSuite"

// ---- render arguments of OpenFX 1.4 / 1.5 that VEGAS gives (Reno renders neither with OpenGL nor OpenCL: 0)
#ifndef kOfxImageEffectPropOpenGLEnabled
#  define kOfxImageEffectPropOpenGLEnabled "OfxImageEffectPropOpenGLEnabled"
#endif
#define kRenoOfxImageEffectPropOpenCLEnabled "OfxImageEffectPropOpenCLEnabled"

// ---- standard names from OpenFX headers that are not among the eight kept in third_party
#ifndef kOfxParamHostPropSupportsParametricAnimation
#  define kOfxParamHostPropSupportsParametricAnimation "OfxParamHostPropSupportsParametricAnimation"
#endif
#ifndef kOfxImageEffectPropOpenGLRenderSupported
#  define kOfxImageEffectPropOpenGLRenderSupported "OfxImageEffectPropOpenGLRenderSupported"
#endif

// ---- standard suites whose headers are not among the eight OpenFX headers kept in third_party
#define kRenoProgressSuite "OfxProgressSuite"
typedef struct RenoProgressSuiteV1 {
    OfxStatus (*progressStart)(void* effectInstance, const char* label);
    OfxStatus (*progressUpdate)(void* effectInstance, double progress);
    OfxStatus (*progressEnd)(void* effectInstance);
} RenoProgressSuiteV1;
typedef struct RenoProgressSuiteV2 {
    OfxStatus (*progressStart)(void* effectInstance, const char* message, const char* messageId);
    OfxStatus (*progressUpdate)(void* effectInstance, double progress);
    OfxStatus (*progressEnd)(void* effectInstance);
} RenoProgressSuiteV2;

#define kRenoTimeLineSuite "OfxTimeLineSuite"
typedef struct RenoTimeLineSuiteV1 {
    OfxStatus (*getTime)(void* instance, double* time);
    OfxStatus (*gotoTime)(void* instance, double time);
    OfxStatus (*getTimeBounds)(void* instance, double* firstTime, double* lastTime);
} RenoTimeLineSuiteV1;
