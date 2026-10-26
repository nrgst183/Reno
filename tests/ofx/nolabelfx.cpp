// A small OpenFX plug-in that gives no label of its own, as some of VEGAS Pro's effects do: the host takes the name from
// the bundle's resource file (Contents/Resources/nolabelfx.xml, written by the tests) or, without one, from the
// identifier. Two filters that copy their source: "reno.nolabel:resourceNamed" (named by the resource file) and
// "reno.nolabel:Filters.unnamedEffect3D" (no label anywhere).
//   g++ -std=c++17 -O2 -fPIC -shared tests/ofx/nolabelfx.cpp -Ithird_party/openfx -o nolabelfx.ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"
#include <cstring>

#ifdef _WIN32
#define NL_EXPORT extern "C" __declspec(dllexport)
#else
#define NL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEff = nullptr;

static OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    (void)in;
    (void)out;
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!std::strcmp(action, kOfxActionLoad)) {
        if (!gHost) return kOfxStatErrMissingHostFeature;
        gProp = (const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
        gEff = (const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
        return gProp && gEff ? kOfxStatOK : kOfxStatErrMissingHostFeature;
    }
    if (!std::strcmp(action, kOfxActionDescribe)) {
        // (no kOfxPropLabel, no grouping, no description)
        OfxPropertySetHandle p = nullptr;
        gEff->getPropertySet(e, &p);
        gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
        gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
        gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        OfxPropertySetHandle c = nullptr;
        gEff->clipDefine(e, kOfxImageEffectSimpleSourceClipName, &c);
        if (c) gProp->propSetString(c, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        gEff->clipDefine(e, kOfxImageEffectOutputClipName, &c);
        if (c) gProp->propSetString(c, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionIsIdentity)) {
        gProp->propSetString(out, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

static void setHost(OfxHost* h) { gHost = h; }

static OfxPlugin gPlugins[] = {
    {kOfxImageEffectPluginApi, 1, "reno.nolabel:resourceNamed", 1, 0, setHost, entry},
    {kOfxImageEffectPluginApi, 1, "reno.nolabel:Filters.unnamedEffect3D", 1, 0, setHost, entry},
};

NL_EXPORT int OfxGetNumberOfPlugins(void) { return 2; }
NL_EXPORT OfxPlugin* OfxGetPlugin(int nth) { return nth >= 0 && nth < 2 ? &gPlugins[nth] : nullptr; }
