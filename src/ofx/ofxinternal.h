// Shared between the files of the OpenFX host; not for the rest of Reno.
#pragma once
#include "ofxhost.h"
#include "vegas_ext.h"
#include <cstring>

namespace reno {
namespace ofx {

const void* fetchSuiteImpl(OfxPropertySetHandle host, const char* name, int version);

// What a bundle's resource file says about an effect (ofxresources.cpp).
struct ResourceLabels { std::string label, group, description; };
bool bundleResourceLabels(const std::string& bundlePath, const std::string& binaryPath, const std::string& id, const std::string& hostName,
                          ResourceLabels& out);

inline PropertySet* asProps(OfxPropertySetHandle h)
{
    PropertySet* p = (PropertySet*)h;
    return (p && p->magic == PropertySet::kMagic) ? p : nullptr;
}
// (EffectBase has a virtual destructor, so its tag is not at offset 0 like the other handles'. The tag is read as plain bytes at its real offset; the pointer is only treated as an EffectBase once the tag matches.)
inline size_t effectTagOffset()
{
    static const size_t off = [] {
        EffectBase probe;
        return (size_t)((const char*)&probe.magic - (const char*)&probe);
    }();
    return off;
}
inline EffectBase* asEffect(const void* h)
{
    if (!h) return nullptr;
    uint32_t tag = 0;
    std::memcpy(&tag, (const char*)h + effectTagOffset(), sizeof tag);
    return tag == EffectBase::kMagic ? (EffectBase*)h : nullptr;
}
inline Instance* asInstance(const void* h)
{
    EffectBase* e = asEffect(h);
    return (e && e->isInstance) ? static_cast<Instance*>(e) : nullptr;
}
inline Param* asParam(OfxParamHandle h)
{
    Param* p = (Param*)h;
    return (p && p->magic == Param::kMagic) ? p : nullptr;
}
inline ParamSet* asParamSet(OfxParamSetHandle h)
{
    ParamSet* p = (ParamSet*)h;
    return (p && p->magic == ParamSet::kMagic) ? p : nullptr;
}
inline ClipInst* asClip(OfxImageClipHandle h)
{
    ClipInst* c = (ClipInst*)h;
    return (c && c->magic == ClipInst::kMagic) ? c : nullptr;
}

// The effect's own window at the top of its panel (VEGAS's "HWnd interact"): what its handle points at.
struct HwndInteractImpl {
    static const uint32_t kMagic = 0x48776e64;
    uint32_t magic = kMagic;
    PropertySet props;
    OfxPluginEntryPoint* entry = nullptr;
    bool instanceMade = false, windowMade = false;
    Instance* overlayOf = nullptr;
    Instance* paramInst = nullptr;
    Param* paramOf = nullptr;
    ~HwndInteractImpl() { magic = 0; }
};
inline HwndInteractImpl* asInteract(void* handle)
{
    HwndInteractImpl* h = (HwndInteractImpl*)handle;
    return (h && h->magic == HwndInteractImpl::kMagic) ? h : nullptr;
}
inline PropertySet* hwndInteractProps(void* handle)
{
    HwndInteractImpl* h = (HwndInteractImpl*)handle;
    return (h && h->magic == HwndInteractImpl::kMagic) ? &h->props : nullptr;
}
// property names from ofxKeySyms.h and the newer ofxInteract.h, which are not among the headers here
#define kRenoOfxPropKeySym "kOfxPropKeySym"
#define kRenoOfxPropKeyString "kOfxPropKeyString"
#define kRenoOfxInteractPropViewportSize "OfxInteractPropViewport"
#define kRenoOfxInteractPropDrawContext "OfxInteractPropDrawContext"
// A plug-in changed a parameter's value itself (through the parameter suite).
void pluginSetParam(Param* p);
// Fills the properties of a frame handed to a plug-in.
void fillImageProps(PropertySet& props, const Image& img, const std::string& id, const double scale[2] = nullptr);
const char* depthString(PixelDepth d);

// What a plug-in asked for and did not get, so a "missing host feature" can be put in plain words. Each thread keeps its own: Plugin::call starts it afresh before an action and reads it afterwards.
struct MissingTrace {
    Plugin* plugin = nullptr;       // the plug-in whose action this thread is in
    std::string suite;
    std::string prop;
};
MissingTrace& missingTrace();
// Called by the property suite's getters when a property is not there (kOfxStatErrUnknown) or has fewer values than
// the index asked for (kOfxStatErrBadIndex, index >= 0). Returns 'st'.
OfxStatus noteMissingProp(PropertySet* s, const char* name, int index, OfxStatus st);
void noteMissingSuite(const char* name, int version);
// Which kind of property set this is, in words ("the host's properties", "a clip", ...).
std::string propSetKind(const PropertySet* s);

}  // namespace ofx
}  // namespace reno
