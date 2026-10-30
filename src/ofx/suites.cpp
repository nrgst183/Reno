// The suites: the functions a plug-in calls back into the host with.
#include "ofxinternal.h"
#include "gpurender.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <set>
#include <mutex>
#include <atomic>
#include <new>
#include <algorithm>

namespace reno {
namespace ofx {

// ====================================================================== property suite
static OfxStatus propSetPointer(OfxPropertySetHandle h, const char* name, int index, void* value)
{
    PropertySet* s = asProps(h);
    if (!s || !name) return kOfxStatErrBadHandle;
    if (index < 0) return kOfxStatErrBadIndex;
    Property& p = s->ensure(name, PType::Pointer);
    if (p.type != PType::Pointer) return kOfxStatErrValue;
    if ((int)p.p.size() <= index) p.p.resize((size_t)index + 1);
    p.p[(size_t)index] = value;
    if (s->onChanged) s->onChanged(name);
    return kOfxStatOK;
}

static OfxStatus propSetString(OfxPropertySetHandle h, const char* name, int index, const char* value)
{
    PropertySet* s = asProps(h);
    if (!s || !name) return kOfxStatErrBadHandle;
    if (index < 0) return kOfxStatErrBadIndex;
    Property& p = s->ensure(name, PType::String);
    if (p.type != PType::String) return kOfxStatErrValue;
    if ((int)p.s.size() <= index) p.s.resize((size_t)index + 1);
    p.s[(size_t)index] = value ? value : "";
    if (s->onChanged) s->onChanged(name);
    return kOfxStatOK;
}

static OfxStatus propSetDouble(OfxPropertySetHandle h, const char* name, int index, double value)
{
    PropertySet* s = asProps(h);
    if (!s || !name) return kOfxStatErrBadHandle;
    if (index < 0) return kOfxStatErrBadIndex;
    Property& p = s->ensure(name, PType::Double);
    if (p.type == PType::Int) {
        if ((int)p.i.size() <= index) p.i.resize((size_t)index + 1);
        p.i[(size_t)index] = (int)std::lround(value);
    } else if (p.type == PType::Double) {
        if ((int)p.d.size() <= index) p.d.resize((size_t)index + 1);
        p.d[(size_t)index] = value;
    } else return kOfxStatErrValue;
    if (s->onChanged) s->onChanged(name);
    return kOfxStatOK;
}

static OfxStatus propSetInt(OfxPropertySetHandle h, const char* name, int index, int value)
{
    PropertySet* s = asProps(h);
    if (!s || !name) return kOfxStatErrBadHandle;
    if (index < 0) return kOfxStatErrBadIndex;
    Property& p = s->ensure(name, PType::Int);
    if (p.type == PType::Double) {
        if ((int)p.d.size() <= index) p.d.resize((size_t)index + 1);
        p.d[(size_t)index] = value;
    } else if (p.type == PType::Int) {
        if ((int)p.i.size() <= index) p.i.resize((size_t)index + 1);
        p.i[(size_t)index] = value;
    } else return kOfxStatErrValue;
    if (s->onChanged) s->onChanged(name);
    return kOfxStatOK;
}

static OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* name, int count, void* const* value)
{
    if (count < 0 || (count > 0 && !value)) return kOfxStatErrValue;
    for (int k = 0; k < count; ++k) { OfxStatus st = propSetPointer(h, name, k, value[k]); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}
static OfxStatus propSetStringN(OfxPropertySetHandle h, const char* name, int count, const char* const* value)
{
    if (count < 0 || (count > 0 && !value)) return kOfxStatErrValue;
    for (int k = 0; k < count; ++k) { OfxStatus st = propSetString(h, name, k, value[k]); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}
static OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* name, int count, const double* value)
{
    if (count < 0 || (count > 0 && !value)) return kOfxStatErrValue;
    for (int k = 0; k < count; ++k) { OfxStatus st = propSetDouble(h, name, k, value[k]); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}
static OfxStatus propSetIntN(OfxPropertySetHandle h, const char* name, int count, const int* value)
{
    if (count < 0 || (count > 0 && !value)) return kOfxStatErrValue;
    for (int k = 0; k < count; ++k) { OfxStatus st = propSetInt(h, name, k, value[k]); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}

static OfxStatus propGetPointer(OfxPropertySetHandle h, const char* name, int index, void** value)
{
    PropertySet* s = asProps(h);
    if (!s || !name || !value) return kOfxStatErrBadHandle;
    Property* p = s->find(name);
    if (!p) return noteMissingProp(s, name, index, kOfxStatErrUnknown);
    if (p->type != PType::Pointer) return kOfxStatErrValue;
    if (index < 0 || index >= (int)p->p.size()) return noteMissingProp(s, name, index, kOfxStatErrBadIndex);
    *value = p->p[(size_t)index];
    return kOfxStatOK;
}

static OfxStatus propGetString(OfxPropertySetHandle h, const char* name, int index, char** value)
{
    PropertySet* s = asProps(h);
    if (!s || !name || !value) return kOfxStatErrBadHandle;
    Property* p = s->find(name);
    if (!p) return noteMissingProp(s, name, index, kOfxStatErrUnknown);
    if (p->type != PType::String) return kOfxStatErrValue;
    if (index < 0 || index >= (int)p->s.size()) return noteMissingProp(s, name, index, kOfxStatErrBadIndex);
    *value = const_cast<char*>(p->s[(size_t)index].c_str());
    return kOfxStatOK;
}

static OfxStatus propGetDouble(OfxPropertySetHandle h, const char* name, int index, double* value)
{
    PropertySet* s = asProps(h);
    if (!s || !name || !value) return kOfxStatErrBadHandle;
    Property* p = s->find(name);
    if (!p) return noteMissingProp(s, name, index, kOfxStatErrUnknown);
    if (p->type == PType::Double) {
        if (index < 0 || index >= (int)p->d.size()) return noteMissingProp(s, name, index, kOfxStatErrBadIndex);
        *value = p->d[(size_t)index];
    } else if (p->type == PType::Int) {
        if (index < 0 || index >= (int)p->i.size()) return noteMissingProp(s, name, index, kOfxStatErrBadIndex);
        *value = p->i[(size_t)index];
    } else return kOfxStatErrValue;
    return kOfxStatOK;
}

static OfxStatus propGetInt(OfxPropertySetHandle h, const char* name, int index, int* value)
{
    PropertySet* s = asProps(h);
    if (!s || !name || !value) return kOfxStatErrBadHandle;
    Property* p = s->find(name);
    if (!p) return noteMissingProp(s, name, index, kOfxStatErrUnknown);
    if (p->type == PType::Int) {
        if (index < 0 || index >= (int)p->i.size()) return noteMissingProp(s, name, index, kOfxStatErrBadIndex);
        *value = p->i[(size_t)index];
    } else if (p->type == PType::Double) {
        if (index < 0 || index >= (int)p->d.size()) return noteMissingProp(s, name, index, kOfxStatErrBadIndex);
        *value = (int)std::lround(p->d[(size_t)index]);
    } else return kOfxStatErrValue;
    return kOfxStatOK;
}

static OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* name, int count, void** value)
{
    for (int k = 0; k < count; ++k) { OfxStatus st = propGetPointer(h, name, k, value + k); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}
static OfxStatus propGetStringN(OfxPropertySetHandle h, const char* name, int count, char** value)
{
    for (int k = 0; k < count; ++k) { OfxStatus st = propGetString(h, name, k, value + k); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}
static OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* name, int count, double* value)
{
    for (int k = 0; k < count; ++k) { OfxStatus st = propGetDouble(h, name, k, value + k); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}
static OfxStatus propGetIntN(OfxPropertySetHandle h, const char* name, int count, int* value)
{
    for (int k = 0; k < count; ++k) { OfxStatus st = propGetInt(h, name, k, value + k); if (st != kOfxStatOK) return st; }
    return kOfxStatOK;
}

static OfxStatus propReset(OfxPropertySetHandle h, const char* name)
{
    PropertySet* s = asProps(h);
    if (!s || !name) return kOfxStatErrBadHandle;
    Property* p = s->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (!std::strcmp(name, kOfxParamPropChoiceOption) || !std::strcmp(name, kOfxParamPropPageChild)) {
        s->resetDim(name, 0);
        if (s->onChanged) s->onChanged(name);
    }
    return kOfxStatOK;
}

static OfxStatus propGetDimension(OfxPropertySetHandle h, const char* name, int* count)
{
    PropertySet* s = asProps(h);
    if (!s || !name || !count) return kOfxStatErrBadHandle;
    Property* p = s->find(name);
    if (!p) return noteMissingProp(s, name, -1, kOfxStatErrUnknown);
    *count = p->dim();
    return kOfxStatOK;
}

static const OfxPropertySuiteV1 gPropSuite = {
    propSetPointer, propSetString, propSetDouble, propSetInt,
    propSetPointerN, propSetStringN, propSetDoubleN, propSetIntN,
    propGetPointer, propGetString, propGetDouble, propGetInt,
    propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,
    propReset, propGetDimension};

// ====================================================================== parameter suite
static OfxStatus paramDefine(OfxParamSetHandle h, const char* paramType, const char* name, OfxPropertySetHandle* propertySet)
{
    ParamSet* ps = asParamSet(h);
    if (!ps || !paramType || !name) return kOfxStatErrBadHandle;
    if (ps->owner && ps->owner->isInstance) return kOfxStatErrBadHandle;        // only while describing
    if (propertySet) *propertySet = nullptr;
    if (ps->find(name)) return kOfxStatErrExists;
    Param* p = ps->define(paramType, name);
    if (!p) {
        // a type Reno does not provide: not kept, the effect is told and is expected to do without
        logf("ofx: parameter '%s' has a type Reno does not know (%s)", name, paramType);
        return !std::strcmp(paramType, kOfxParamTypeStrChoice) ? kOfxStatErrUnsupported : kOfxStatErrUnknown;
    }
    if (propertySet) *propertySet = (OfxPropertySetHandle)&p->props;
    return kOfxStatOK;
}

static OfxStatus paramGetHandle(OfxParamSetHandle h, const char* name, OfxParamHandle* param, OfxPropertySetHandle* propertySet)
{
    ParamSet* ps = asParamSet(h);
    if (!ps || !name) return kOfxStatErrBadHandle;
    Param* p = ps->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (param) *param = (OfxParamHandle)p;
    if (propertySet) *propertySet = (OfxPropertySetHandle)&p->props;
    return kOfxStatOK;
}

static OfxStatus paramSetGetPropertySet(OfxParamSetHandle h, OfxPropertySetHandle* propHandle)
{
    ParamSet* ps = asParamSet(h);
    if (!ps || !propHandle) return kOfxStatErrBadHandle;
    *propHandle = (OfxPropertySetHandle)&ps->props;
    return kOfxStatOK;
}

static OfxStatus paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle* propHandle)
{
    Param* p = asParam(h);
    if (!p || !propHandle) return kOfxStatErrBadHandle;
    *propHandle = (OfxPropertySetHandle)&p->props;
    return kOfxStatOK;
}

static double instanceTime(Param* p)
{
    Instance* inst = (p->set && p->set->owner) ? asInstance(p->set->owner) : nullptr;
    return inst ? inst->currentTime : 0.0;
}

// While an effect without multi-resolution renders at a preview's size (Instance::canonW), settings in canonical coordinates (positions and sizes,
// the X / Y / XY double types) are scaled to that size. The factor for dimension k of 'p' (1 for everything else).
static double canonicalFactor(Param* p, int k)
{
    Instance* inst = (p->set && p->set->owner) ? asInstance(p->set->owner) : nullptr;
    if (!inst || inst->canonW <= 0) return 1.0;
    if (p->kind != ParamKind::Double && p->kind != ParamKind::Double2D) return 1.0;
    const std::string t = p->props.getString(kOfxParamPropDoubleType);
    if (t == kOfxParamDoubleTypeX || t == kOfxParamDoubleTypeXAbsolute) return inst->canonScale[0];
    if (t == kOfxParamDoubleTypeY || t == kOfxParamDoubleTypeYAbsolute) return inst->canonScale[1];
    if (t == kOfxParamDoubleTypeXY || t == kOfxParamDoubleTypeXYAbsolute) return inst->canonScale[k == 0 ? 0 : 1];
    return 1.0;
}

static OfxStatus getV(Param* p, const std::vector<double>& v, va_list ap, double time)
{
    switch (p->kind) {
    case ParamKind::Int: case ParamKind::Bool: case ParamKind::Choice: case ParamKind::Int2D: case ParamKind::Int3D:
        for (int k = 0; k < p->dim; ++k) {
            int* out = va_arg(ap, int*);
            if (out) *out = (int)std::lround(k < (int)v.size() ? v[(size_t)k] : 0.0);
        }
        return kOfxStatOK;
    case ParamKind::Double: case ParamKind::Double2D: case ParamKind::Double3D: case ParamKind::RGB: case ParamKind::RGBA:
        for (int k = 0; k < p->dim; ++k) {
            double* out = va_arg(ap, double*);
            if (out) *out = (k < (int)v.size() ? v[(size_t)k] : 0.0) * canonicalFactor(p, k);
        }
        return kOfxStatOK;
    case ParamKind::String: case ParamKind::Custom: {
        char** out = va_arg(ap, char**);
        // the text lives until a few more calls have been made on this thread
        static thread_local std::string ring[16];
        static thread_local unsigned next = 0;
        std::string& keep = ring[next++ & 15];
        keep = p->strAt(time);
        if (out) *out = const_cast<char*>(keep.c_str());
        return kOfxStatOK;
    }
    default:
        return kOfxStatErrUnsupported;
    }
}

static OfxStatus paramGetValue(OfxParamHandle h, ...)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, h);
    const double t = instanceTime(p);
    OfxStatus st = getV(p, p->valueAt(t), ap, t);
    va_end(ap);
    return st;
}

static OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime time, ...)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    OfxStatus st = getV(p, p->valueAt(time), ap, time);
    va_end(ap);
    return st;
}

static OfxStatus paramGetDerivative(OfxParamHandle h, OfxTime time, ...)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    if (p->dim == 0) return kOfxStatErrUnsupported;
    std::vector<double> a = p->valueAt(time - 0.5), b = p->valueAt(time + 0.5), dv((size_t)p->dim);
    for (int k = 0; k < p->dim; ++k) dv[(size_t)k] = b[(size_t)k] - a[(size_t)k];
    va_list ap;
    va_start(ap, time);
    OfxStatus st = kOfxStatOK;
    for (int k = 0; k < p->dim; ++k) {
        if (p->isWhole()) { int* out = va_arg(ap, int*); if (out) *out = (int)std::lround(dv[(size_t)k]); }
        else { double* out = va_arg(ap, double*); if (out) *out = dv[(size_t)k] * canonicalFactor(p, k); }
    }
    va_end(ap);
    return st;
}

static OfxStatus paramGetIntegral(OfxParamHandle h, OfxTime time1, OfxTime time2, ...)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    if (p->dim == 0) return kOfxStatErrUnsupported;
    std::vector<double> sum((size_t)p->dim, 0.0);
    const int steps = p->keys.empty() ? 1 : 64;
    const double dt = (time2 - time1) / steps;
    for (int s = 0; s < steps; ++s) {
        std::vector<double> v = p->valueAt(time1 + (s + 0.5) * dt);
        for (int k = 0; k < p->dim; ++k) sum[(size_t)k] += v[(size_t)k] * dt;
    }
    va_list ap;
    va_start(ap, time2);
    for (int k = 0; k < p->dim; ++k) {
        if (p->isWhole()) { int* out = va_arg(ap, int*); if (out) *out = (int)std::lround(sum[(size_t)k]); }
        else { double* out = va_arg(ap, double*); if (out) *out = sum[(size_t)k] * canonicalFactor(p, k); }
    }
    va_end(ap);
    return kOfxStatOK;
}

static OfxStatus setV(Param* p, bool atTime, double time, va_list ap)
{
    if (p->kind == ParamKind::String || p->kind == ParamKind::Custom) {
        const char* s = va_arg(ap, const char*);
        const std::string text = s ? s : "";
        if (atTime && p->props.getInt(kOfxParamPropAnimates)) p->strKeys[time] = text;
        else if (!p->strKeys.empty()) p->strKeys[atTime ? time : instanceTime(p)] = text;
        else p->str = text;
        p->props.setInt(kOfxParamPropIsAnimating, p->strKeys.empty() ? 0 : 1);
        pluginSetParam(p);
        return kOfxStatOK;
    }
    if (p->dim == 0) return kOfxStatErrUnsupported;
    std::vector<double> v((size_t)p->dim);
    for (int k = 0; k < p->dim; ++k) {
        if (p->isWhole()) v[(size_t)k] = va_arg(ap, int);
        else v[(size_t)k] = va_arg(ap, double);
    }
    if (atTime && p->props.getInt(kOfxParamPropAnimates)) p->keys[time] = v;
    else if (!p->keys.empty()) p->keys[atTime ? time : instanceTime(p)] = v;
    else p->value = v;
    p->props.setInt(kOfxParamPropIsAnimating, p->keys.empty() ? 0 : 1);
    pluginSetParam(p);
    return kOfxStatOK;
}

static OfxStatus paramSetValue(OfxParamHandle h, ...)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, h);
    OfxStatus st = setV(p, false, 0.0, ap);
    va_end(ap);
    return st;
}

static OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime time, ...)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    OfxStatus st = setV(p, true, time, ap);
    va_end(ap);
    return st;
}

static OfxStatus paramGetNumKeys(OfxParamHandle h, unsigned int* numberOfKeys)
{
    Param* p = asParam(h);
    if (!p || !numberOfKeys) return kOfxStatErrBadHandle;
    *numberOfKeys = (unsigned int)p->keyCount();
    return kOfxStatOK;
}

static OfxStatus paramGetKeyTime(OfxParamHandle h, unsigned int nthKey, OfxTime* time)
{
    Param* p = asParam(h);
    if (!p || !time) return kOfxStatErrBadHandle;
    if (nthKey >= p->keyCount()) return kOfxStatErrBadIndex;
    if (p->isText()) {
        auto it = p->strKeys.begin();
        std::advance(it, nthKey);
        *time = it->first;
        return kOfxStatOK;
    }
    auto it = p->keys.begin();
    std::advance(it, nthKey);
    *time = it->first;
    return kOfxStatOK;
}

static OfxStatus paramGetKeyIndex(OfxParamHandle h, OfxTime time, int direction, int* index)
{
    Param* p = asParam(h);
    if (!index) return kOfxStatErrBadHandle;
    *index = -1;
    if (!p) return kOfxStatErrBadHandle;
    int n = 0, found = -1;
    std::vector<double> times;
    if (p->isText()) for (const auto& kv : p->strKeys) times.push_back(kv.first);
    else for (const auto& kv : p->keys) times.push_back(kv.first);
    for (auto it = times.begin(); it != times.end(); ++it, ++n) {
        if (direction == 0) { if (*it == time) { found = n; break; } }
        else if (direction < 0) { if (*it < time) found = n; else break; }
        else if (*it > time) { found = n; break; }
    }
    if (found < 0) return kOfxStatFailed;
    *index = found;
    return kOfxStatOK;
}

static OfxStatus paramDeleteKey(OfxParamHandle h, OfxTime time)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    if (p->isText()) {
        auto st = p->strKeys.find(time);
        if (st == p->strKeys.end()) return kOfxStatErrBadIndex;
        if (p->strKeys.size() == 1) p->str = st->second;
        p->strKeys.erase(st);
        p->props.setInt(kOfxParamPropIsAnimating, p->strKeys.empty() ? 0 : 1);
        pluginSetParam(p);
        return kOfxStatOK;
    }
    auto it = p->keys.find(time);
    if (it == p->keys.end()) return kOfxStatErrBadIndex;
    if (p->keys.size() == 1) p->value = it->second;
    p->keyCurves.erase(it->first);
    p->keys.erase(it);
    p->props.setInt(kOfxParamPropIsAnimating, p->keys.empty() ? 0 : 1);
    pluginSetParam(p);
    return kOfxStatOK;
}

static OfxStatus paramDeleteAllKeys(OfxParamHandle h)
{
    Param* p = asParam(h);
    if (!p) return kOfxStatErrBadHandle;
    if (!p->keys.empty()) p->value = p->valueAt(instanceTime(p));
    p->keys.clear();
    p->keyCurves.clear();
    if (!p->strKeys.empty()) p->str = p->strAt(instanceTime(p));
    p->strKeys.clear();
    p->props.setInt(kOfxParamPropIsAnimating, 0);
    pluginSetParam(p);
    return kOfxStatOK;
}

static OfxStatus paramCopy(OfxParamHandle to, OfxParamHandle from, OfxTime dstOffset, const OfxRangeD* frameRange)
{
    Param* a = asParam(to);
    Param* b = asParam(from);
    if (!a || !b) return kOfxStatErrBadHandle;
    if (a->kind != b->kind) return kOfxStatErrValue;
    a->value = b->value;
    a->str = b->str;
    a->keys.clear();
    for (const auto& kv : b->keys) {
        if (frameRange && (kv.first < frameRange->min || kv.first > frameRange->max)) continue;
        a->keys[kv.first + dstOffset] = kv.second;
    }
    a->strKeys.clear();
    for (const auto& kv : b->strKeys) {
        if (frameRange && (kv.first < frameRange->min || kv.first > frameRange->max)) continue;
        a->strKeys[kv.first + dstOffset] = kv.second;
    }
    a->props.setInt(kOfxParamPropIsAnimating, (a->keys.empty() && a->strKeys.empty()) ? 0 : 1);
    pluginSetParam(a);
    return kOfxStatOK;
}

static OfxStatus paramEditBegin(OfxParamSetHandle h, const char*) { return asParamSet(h) ? kOfxStatOK : kOfxStatErrBadHandle; }
static OfxStatus paramEditEnd(OfxParamSetHandle h) { return asParamSet(h) ? kOfxStatOK : kOfxStatErrBadHandle; }

static const OfxParameterSuiteV1 gParamSuite = {
    paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet,
    paramGetValue, paramGetValueAtTime, paramGetDerivative, paramGetIntegral,
    paramSetValue, paramSetValueAtTime, paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex,
    paramDeleteKey, paramDeleteAllKeys, paramCopy, paramEditBegin, paramEditEnd};

// ====================================================================== image effect suite
static OfxStatus fxGetPropertySet(OfxImageEffectHandle h, OfxPropertySetHandle* propHandle)
{
    EffectBase* e = asEffect(h);
    if (!e || !propHandle) return kOfxStatErrBadHandle;
    *propHandle = (OfxPropertySetHandle)&e->props;
    return kOfxStatOK;
}

static OfxStatus fxGetParamSet(OfxImageEffectHandle h, OfxParamSetHandle* paramSet)
{
    EffectBase* e = asEffect(h);
    if (!e || !paramSet) return kOfxStatErrBadHandle;
    *paramSet = (OfxParamSetHandle)&e->params;
    return kOfxStatOK;
}

static OfxStatus fxClipDefine(OfxImageEffectHandle h, const char* name, OfxPropertySetHandle* propertySet)
{
    EffectBase* e = asEffect(h);
    if (!e || e->isInstance || !name) return kOfxStatErrBadHandle;
    Descriptor* d = static_cast<Descriptor*>(e);
    ClipDesc* c = d->findClip(name);
    if (!c) {
        d->clips.emplace_back(new ClipDesc());
        c = d->clips.back().get();
        c->name = name;
        PropertySet& s = c->props;
        s.setString(kOfxPropType, kOfxTypeClip);
        s.setString(kOfxPropName, name);
        s.setString(kOfxPropLabel, name);
        s.setString(kOfxPropShortLabel, name);
        s.setString(kOfxPropLongLabel, name);
        s.ensure(kOfxImageEffectPropSupportedComponents, PType::String);
        s.setInt(kOfxImageEffectPropTemporalClipAccess, 0);
        s.setInt(kOfxImageClipPropOptional, 0);
        s.setString(kOfxImageClipPropFieldExtraction, kOfxImageFieldDoubled);
        s.setInt(kOfxImageClipPropIsMask, 0);
        s.setInt(kOfxImageEffectPropSupportsTiles, 1);
    }
    if (propertySet) *propertySet = (OfxPropertySetHandle)&c->props;
    return kOfxStatOK;
}

static OfxStatus fxClipGetHandle(OfxImageEffectHandle h, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* propertySet)
{
    Instance* inst = asInstance(h);
    if (!inst || !name) return kOfxStatErrBadHandle;
    ClipInst* c = inst->findClip(name);
    if (!c) return kOfxStatErrBadHandle;
    if (clip) *clip = (OfxImageClipHandle)c;
    if (propertySet) *propertySet = (OfxPropertySetHandle)&c->props;
    return kOfxStatOK;
}

static OfxStatus fxClipGetPropertySet(OfxImageClipHandle h, OfxPropertySetHandle* propHandle)
{
    ClipInst* c = asClip(h);
    if (!c || !propHandle) return kOfxStatErrBadHandle;
    *propHandle = (OfxPropertySetHandle)&c->props;
    return kOfxStatOK;
}

static OfxStatus fxClipGetImage(OfxImageClipHandle h, OfxTime time, const OfxRectD*, OfxPropertySetHandle* imageHandle)
{
    if (imageHandle) *imageHandle = nullptr;
    ClipInst* c = asClip(h);
    if (!c || !imageHandle) return kOfxStatErrBadHandle;
    Instance* inst = c->inst;
    ImagePtr img;
    if (c->isOutput) img = inst->outputImage;
    else if (c->source) {
        img = c->source(time);
        if (img && img->depth != inst->depth()) img = img->converted(inst->depth());
        // a frame only on the card (Image::gpuHold) goes on to a built-in effect that takes it from there; anything else reads the pixels
        if (img && img->gpuHold && (c->isOutput || !inst->gpuChainable())) img->sync();
    }
    if (!img) return kOfxStatFailed;
    std::unique_ptr<ImageHandle> ih(new ImageHandle());
    ih->img = img;
    ih->props.owner = ih.get();
    fillImageProps(ih->props, *img, strfmt("%s@%.4f#%p", c->name.c_str(), time, (void*)img.get()), inst->renderScale);
    *imageHandle = (OfxPropertySetHandle)&ih->props;
    std::lock_guard<std::mutex> lk(inst->imageMutex);
    inst->liveImages.push_back(std::move(ih));
    return kOfxStatOK;
}

static OfxStatus fxClipReleaseImage(OfxPropertySetHandle imageHandle)
{
    PropertySet* s = asProps(imageHandle);
    if (!s) return kOfxStatErrBadHandle;
    if (s->getString(kOfxPropType) != kOfxTypeImage || !s->owner) return kOfxStatErrBadHandle;
    ImageHandle* ih = (ImageHandle*)s->owner;
    if (ih->released) return kOfxStatErrBadHandle;
    ih->released = true;
    ih->img.reset();
    s->setPointer(kOfxImagePropData, nullptr);
    return kOfxStatOK;
}

static OfxStatus fxClipGetRegionOfDefinition(OfxImageClipHandle h, OfxTime, OfxRectD* bounds)
{
    ClipInst* c = asClip(h);
    if (!c || !bounds) return kOfxStatErrBadHandle;
    bounds->x1 = 0;
    bounds->y1 = 0;
    bounds->x2 = c->inst->canonicalWidth();
    bounds->y2 = c->inst->canonicalHeight();
    return kOfxStatOK;
}

static int fxAbort(OfxImageEffectHandle h)
{
    Instance* inst = asInstance(h);
    return (inst && inst->aborted()) ? 1 : 0;
}

struct ImageMem {
    void* ptr;
    size_t size;
    int lockCount;
};
// Image memory handles are looked up in a registry so a freed or garbage handle is refused instead of dereferenced.
static std::mutex gImageMemMutex;
static std::set<ImageMem*>& imageMemSet()
{
    static std::set<ImageMem*> s;
    return s;
}

static OfxStatus fxImageMemoryAlloc(OfxImageEffectHandle, size_t nBytes, OfxImageMemoryHandle* memoryHandle)
{
    if (!memoryHandle) return kOfxStatErrBadHandle;
    *memoryHandle = nullptr;
    if (nBytes > ((size_t)1 << 40)) return kOfxStatErrMemory;
    ImageMem* m = (ImageMem*)std::malloc(sizeof(ImageMem));
    if (!m) return kOfxStatErrMemory;
    m->ptr = std::malloc(nBytes ? nBytes : 1);
    if (!m->ptr) { std::free(m); return kOfxStatErrMemory; }
    m->size = nBytes;
    m->lockCount = 0;
    {
        std::lock_guard<std::mutex> lk(gImageMemMutex);
        imageMemSet().insert(m);
    }
    *memoryHandle = (OfxImageMemoryHandle)m;
    return kOfxStatOK;
}

static OfxStatus fxImageMemoryFree(OfxImageMemoryHandle h)
{
    ImageMem* m = (ImageMem*)h;
    {
        std::lock_guard<std::mutex> lk(gImageMemMutex);
        if (!m || imageMemSet().erase(m) == 0) return kOfxStatErrBadHandle;
    }
    std::free(m->ptr);
    std::free(m);
    return kOfxStatOK;
}

static OfxStatus fxImageMemoryLock(OfxImageMemoryHandle h, void** returnedPtr)
{
    ImageMem* m = (ImageMem*)h;
    if (!returnedPtr) return kOfxStatErrBadHandle;
    *returnedPtr = nullptr;
    std::lock_guard<std::mutex> lk(gImageMemMutex);
    if (!m || !imageMemSet().count(m)) return kOfxStatErrBadHandle;
    m->lockCount++;
    *returnedPtr = m->ptr;
    return kOfxStatOK;
}

static OfxStatus fxImageMemoryUnlock(OfxImageMemoryHandle h)
{
    ImageMem* m = (ImageMem*)h;
    std::lock_guard<std::mutex> lk(gImageMemMutex);
    if (!m || !imageMemSet().count(m)) return kOfxStatErrBadHandle;
    if (m->lockCount > 0) m->lockCount--;
    return kOfxStatOK;
}

static const OfxImageEffectSuiteV1 gEffectSuite = {
    fxGetPropertySet, fxGetParamSet, fxClipDefine, fxClipGetHandle, fxClipGetPropertySet,
    fxClipGetImage, fxClipReleaseImage, fxClipGetRegionOfDefinition, fxAbort,
    fxImageMemoryAlloc, fxImageMemoryFree, fxImageMemoryLock, fxImageMemoryUnlock};

// ====================================================================== memory suite
// Blocks given out by the memory suite are remembered so that a free of anything else (or a second free) is refused instead of corrupting the heap.
static std::mutex gMemMutex;
static std::set<void*>& memBlocks()
{
    static std::set<void*> s;
    return s;
}
static OfxStatus memAlloc(void*, size_t nBytes, void** allocatedData)
{
    if (!allocatedData) return kOfxStatErrBadHandle;
    *allocatedData = nullptr;
    if (nBytes > ((size_t)1 << 40)) return kOfxStatErrMemory;
    void* p = std::malloc(nBytes ? nBytes : 1);
    if (!p) return kOfxStatErrMemory;
    {
        std::lock_guard<std::mutex> lk(gMemMutex);
        memBlocks().insert(p);
    }
    *allocatedData = p;
    return kOfxStatOK;
}
static OfxStatus memFree(void* allocatedData)
{
    if (!allocatedData) return kOfxStatOK;
    {
        std::lock_guard<std::mutex> lk(gMemMutex);
        if (memBlocks().erase(allocatedData) == 0) return kOfxStatErrBadHandle;
    }
    std::free(allocatedData);
    return kOfxStatOK;
}
static const OfxMemorySuiteV1 gMemorySuite = {memAlloc, memFree};

// ====================================================================== multi-thread suite
static thread_local unsigned int tThreadIndex = 0;
static thread_local bool tSpawned = false;

static OfxStatus mtMultiThread(OfxThreadFunctionV1 func, unsigned int nThreads, void* customArg)
{
    if (!func) return kOfxStatFailed;
    if (tSpawned) return kOfxStatErrExists;
    if (nThreads < 1) nThreads = 1;
    if (nThreads > 256) nThreads = 256;
    std::vector<std::thread> threads;
    for (unsigned int k = 1; k < nThreads; ++k) {
        threads.emplace_back([func, k, nThreads, customArg] {
            tSpawned = true;
            tThreadIndex = k;
            func(k, nThreads, customArg);
        });
    }
    tSpawned = true;
    tThreadIndex = 0;
    func(0, nThreads, customArg);
    tSpawned = false;
    for (auto& t : threads) t.join();
    return kOfxStatOK;
}

static OfxStatus mtNumCPUs(unsigned int* nCPUs)
{
    if (!nCPUs) return kOfxStatFailed;
    *nCPUs = (unsigned int)processorCount();       // reno_cli --threads can make it fewer
    return kOfxStatOK;
}

static OfxStatus mtIndex(unsigned int* threadIndex)
{
    if (!threadIndex) return kOfxStatFailed;
    *threadIndex = tThreadIndex;
    return kOfxStatOK;
}

static int mtIsSpawned(void) { return tSpawned ? 1 : 0; }

// Mutex handles point at structures never returned to the heap (destroyed ones go to a free list), so a stale handle is recognised by its tag.
struct MtMutex {
    std::atomic<uint32_t> tag;
    std::recursive_mutex m;
};
static const uint32_t kMutexLive = 0x4d747831, kMutexDead = 0;
static std::mutex gMutexPoolLock;
static std::vector<MtMutex*>& mutexPool()
{
    static std::vector<MtMutex*>* v = new std::vector<MtMutex*>();
    return *v;
}
static MtMutex* asMutex(const OfxMutexHandle h)
{
    MtMutex* m = (MtMutex*)h;
    return (m && m->tag.load() == kMutexLive) ? m : nullptr;
}

static OfxStatus mtMutexCreate(OfxMutexHandle* mutex, int lockCount)
{
    if (!mutex) return kOfxStatFailed;
    *mutex = nullptr;
    MtMutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(gMutexPoolLock);
        if (!mutexPool().empty()) { m = mutexPool().back(); mutexPool().pop_back(); }
    }
    if (!m) {
        m = new (std::nothrow) MtMutex();
        if (!m) return kOfxStatErrMemory;
    }
    for (int k = 0; k < lockCount; ++k) m->m.lock();
    m->tag.store(kMutexLive);
    *mutex = (OfxMutexHandle)m;
    return kOfxStatOK;
}
static OfxStatus mtMutexDestroy(const OfxMutexHandle mutex)
{
    MtMutex* m = (MtMutex*)mutex;
    if (!m) return kOfxStatErrBadHandle;
    uint32_t live = kMutexLive;
    if (!m->tag.compare_exchange_strong(live, kMutexDead)) return kOfxStatErrBadHandle;
    // a mutex still held by someone else is left alone (never reused) rather than risk undefined behaviour
    if (m->m.try_lock()) {
        m->m.unlock();
        std::lock_guard<std::mutex> lk(gMutexPoolLock);
        mutexPool().push_back(m);
    }
    return kOfxStatOK;
}
static OfxStatus mtMutexLock(const OfxMutexHandle mutex)
{
    MtMutex* m = asMutex(mutex);
    if (!m) return kOfxStatErrBadHandle;
    m->m.lock();
    return kOfxStatOK;
}
static OfxStatus mtMutexUnLock(const OfxMutexHandle mutex)
{
    MtMutex* m = asMutex(mutex);
    if (!m) return kOfxStatErrBadHandle;
    // unlocking a mutex this thread does not hold is undefined for std::mutex; the plug-in is trusted to pair its calls
    m->m.unlock();
    return kOfxStatOK;
}
static OfxStatus mtMutexTryLock(const OfxMutexHandle mutex)
{
    MtMutex* m = asMutex(mutex);
    if (!m) return kOfxStatErrBadHandle;
    return m->m.try_lock() ? kOfxStatOK : kOfxStatFailed;
}

static const OfxMultiThreadSuiteV1 gThreadSuite = {
    mtMultiThread, mtNumCPUs, mtIndex, mtIsSpawned, mtMutexCreate, mtMutexDestroy, mtMutexLock, mtMutexUnLock, mtMutexTryLock};

// VEGAS Pro 18's multithread suite (vegas180.exe, table at 0x1410fb560): multiThread calls the function once on the calling thread as thread 0 of 1 and answers kOfxStatFailed; multiThreadNumCPUs writes 2 and answers
// kOfxStatFailed (the support library takes that as "one processor" and never splits the work); multiThreadIndex answers kOfxStatFailed; multiThreadIsSpawnedThread is always 0. Some stock effects are wrong when really
// split (Cookie Cutter cuts its shape out of each band of rows as if the band were the frame), so VEGAS's own effects (isVegasOwnEffect) get this suite and everything else the real one above.
static OfxStatus vmtMultiThread(OfxThreadFunctionV1 func, unsigned int, void* customArg)
{
    if (!func) return kOfxStatFailed;
    if (tSpawned) return kOfxStatErrExists;
    func(0, 1, customArg);
    return kOfxStatFailed;
}
static OfxStatus vmtNumCPUs(unsigned int* nCPUs)
{
    if (nCPUs) *nCPUs = 2;
    return kOfxStatFailed;
}
static OfxStatus vmtIndex(unsigned int*) { return kOfxStatFailed; }
static int vmtIsSpawned(void) { return 0; }
static const OfxMultiThreadSuiteV1 gVegasThreadSuite = {
    vmtMultiThread, vmtNumCPUs, vmtIndex, vmtIsSpawned, mtMutexCreate, mtMutexDestroy, mtMutexLock, mtMutexUnLock, mtMutexTryLock};

bool isVegasOwnEffect(const std::string& id)
{
    for (const char* p : {"com.vegascreativesoftware", "com.sonycreativesoftware", "de.magix"})
        if (id.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

// ====================================================================== message suite
static OfxStatus messageV(void* handle, const char* messageType, const char* messageId, const char* format, va_list ap)
{
    std::string text = format ? vstrfmt(format, ap) : std::string();
    EffectBase* e = asEffect(handle);
    const char* who = (e && e->plugin) ? e->plugin->info.id.c_str() : "plug-in";
    logf("ofx message [%s] %s%s%s: %s", who, messageType ? messageType : "", messageId ? " " : "", messageId ? messageId : "", text.c_str());
    if (messageType && !std::strcmp(messageType, kOfxMessageQuestion)) return kOfxStatReplyYes;
    return kOfxStatOK;
}

static OfxStatus msgMessage(void* handle, const char* messageType, const char* messageId, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    OfxStatus st = messageV(handle, messageType, messageId, format, ap);
    va_end(ap);
    return st;
}

static OfxStatus msgSetPersistent(void* handle, const char* messageType, const char* messageId, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    std::string text = format ? vstrfmt(format, ap) : std::string();
    va_end(ap);
    EffectBase* eff = asEffect(handle);
    if (!eff) return kOfxStatErrBadHandle;
    Instance* inst = asInstance(handle);
    if (inst) inst->persistentMessage = text;
    logf("ofx message (kept) %s%s%s: %s", messageType ? messageType : "", messageId ? " " : "", messageId ? messageId : "", text.c_str());
    return kOfxStatOK;
}

static OfxStatus msgClearPersistent(void* handle)
{
    if (!asEffect(handle)) return kOfxStatErrBadHandle;
    Instance* inst = asInstance(handle);
    if (inst) inst->persistentMessage.clear();
    return kOfxStatOK;
}

static const OfxMessageSuiteV1 gMessageSuiteV1 = {msgMessage};
static const OfxMessageSuiteV2 gMessageSuiteV2 = {msgMessage, msgSetPersistent, msgClearPersistent};

// ====================================================================== interact suite
// the host swaps nothing: the overlay is drawn into a frame it reads back when the draw action returns
static OfxStatus intSwapBuffers(OfxInteractHandle h) { return asInteract((void*)h) ? kOfxStatOK : kOfxStatErrBadHandle; }
static OfxStatus intRedraw(OfxInteractHandle h)
{
    HwndInteractImpl* i = asInteract((void*)h);
    if (!i) return kOfxStatErrBadHandle;
    if (i->overlayOf && i->overlayOf->onOverlayRedraw) i->overlayOf->onOverlayRedraw();
    if (i->paramInst && i->paramOf && i->paramInst->onParamInteractRedraw) i->paramInst->onParamInteractRedraw(i->paramOf);
    return kOfxStatOK;
}
static OfxStatus intGetPropertySet(OfxInteractHandle h, OfxPropertySetHandle* property)
{
    PropertySet* s = hwndInteractProps((void*)h);
    if (!s || !property) return kOfxStatErrBadHandle;
    *property = (OfxPropertySetHandle)s;
    return kOfxStatOK;
}
static const OfxInteractSuiteV1 gInteractSuite = {intSwapBuffers, intRedraw, intGetPropertySet};

// ====================================================================== progress / timeline
static OfxStatus progStart1(void*, const char* label) { if (label && *label) logf("ofx progress: %s", label); return kOfxStatOK; }
static OfxStatus progStart2(void*, const char* message, const char*) { if (message && *message) logf("ofx progress: %s", message); return kOfxStatOK; }
static OfxStatus progStartV2(void*, const char* message, const char*, int) { if (message && *message) logf("ofx progress: %s", message); return kOfxStatOK; }
static OfxStatus progUpdate(void* h, double)
{
    Instance* inst = asInstance(h);
    return (inst && inst->aborted()) ? kOfxStatReplyNo : kOfxStatOK;
}
static OfxStatus progEnd(void*) { return kOfxStatOK; }

static const RenoProgressSuiteV1 gProgressV1 = {progStart1, progUpdate, progEnd};
static const RenoProgressSuiteV2 gProgressV2 = {progStart2, progUpdate, progEnd};
static const RenoVegasProgressSuiteV1 gVegasProgressV1 = {progStart2, progUpdate, progEnd};
static const RenoVegasProgressSuiteV2 gVegasProgressV2 = {progStartV2, progUpdate, progEnd};

static OfxStatus tlGetTime(void* h, double* time)
{
    Instance* inst = asInstance(h);
    if (!time) return kOfxStatErrBadHandle;
    *time = inst ? inst->currentTime : 0.0;
    return kOfxStatOK;
}
static OfxStatus tlGotoTime(void*, double) { return kOfxStatOK; }
static OfxStatus tlGetTimeBounds(void* h, double* firstTime, double* lastTime)
{
    Instance* inst = asInstance(h);
    if (firstTime) *firstTime = 0.0;
    if (lastTime) *lastTime = inst ? inst->setup.durationFrames : 0.0;
    return kOfxStatOK;
}
static const RenoTimeLineSuiteV1 gTimeLineSuite = {tlGetTime, tlGotoTime, tlGetTimeBounds};

// ====================================================================== VEGAS suites
static OfxStatus hwGetPropertySet(OfxInteractHandle h, OfxPropertySetHandle* property)
{
    PropertySet* s = hwndInteractProps((void*)h);
    if (!s || !property) return kOfxStatErrBadHandle;
    *property = (OfxPropertySetHandle)s;
    return kOfxStatOK;
}
static OfxStatus hwUpdate(OfxInteractHandle h) { return hwndInteractProps((void*)h) ? kOfxStatOK : kOfxStatErrBadHandle; }
static const RenoVegasHWndInteractSuiteV1 gHwndSuite = {hwGetPropertySet, hwUpdate};

static OfxStatus stereoGetImage(OfxImageClipHandle clip, OfxTime time, int, OfxRectD* region, OfxPropertySetHandle* imageHandle)
{
    return fxClipGetImage(clip, time, region, imageHandle);
}
static const RenoVegasStereoscopicSuiteV1 gStereoSuite = {stereoGetImage};

static OfxStatus kfGetInterp(OfxParamHandle h, OfxTime, char** interpolationType)
{
    Param* p = asParam(h);
    if (!p || !interpolationType) return kOfxStatErrBadHandle;
    static char holdText[] = kVegasKeyInterpHold;
    static char linearText[] = kVegasKeyInterpLinear;
    *interpolationType = (p->kind == ParamKind::Bool || p->kind == ParamKind::Choice) ? holdText : linearText;
    return kOfxStatOK;
}
static OfxStatus kfGetSlopes(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
static OfxStatus kfSetInterp(OfxParamHandle h, OfxTime, const char*) { return asParam(h) ? kOfxStatOK : kOfxStatErrBadHandle; }
static OfxStatus kfSetSlopes(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
static const RenoVegasKeyframeSuiteV1 gKeyframeSuite = {kfGetInterp, kfGetSlopes, kfSetInterp, kfSetSlopes};

// A stand-in for suites whose functions Reno does not have: every entry says "unsupported". The argument lists
// differ, but on x64 the caller owns the stack, so one function with no arguments can stand for all of them.
static OfxStatus stubUnsupported(void) { return kOfxStatErrUnsupported; }
typedef OfxStatus (*StubFn)(void);
static const StubFn gStubSuite[48] = {
    stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported,
    stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported,
    stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported,
    stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported,
    stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported,
    stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported, stubUnsupported};

// ---------------------------------------------------------------------- OfxVegasEffectSuite (an effect asks for its Help / About)
static OfxStatus vegasEffectAction(OfxImageEffectHandle h, const char* action)
{
    // as VEGAS: no effect, or an action it does not know -> kOfxStatFailed; Help and About -> kOfxStatOK
    EffectBase* e = asEffect(h);
    if (!e || !action) return kOfxStatFailed;
    if (std::strcmp(action, kVegasActionShowEffectHelp) != 0 && std::strcmp(action, kVegasActionShowEffectAbout) != 0) return kOfxStatFailed;
    Host& host = Host::get();
    if (host.onEffectRequest) host.onEffectRequest(e, action);
    else logf("ofx: %s asked the host for %s", e->plugin ? e->plugin->info.id.c_str() : "?", action);
    return kOfxStatOK;
}
static const RenoVegasEffectSuiteV1 gVegasEffectSuite = {vegasEffectAction};

// ---------------------------------------------------------------------- OfxHWndOverlayInteractSuite
static OfxStatus hwoGetPropertySet(void* h, OfxPropertySetHandle* property)
{
    PropertySet* p = hwndInteractProps(h);
    if (!p || !property) return kOfxStatErrBadHandle;
    *property = (OfxPropertySetHandle)p;
    return kOfxStatOK;
}
static OfxStatus hwoRedraw(void* h)
{
    HwndInteractImpl* i = asInteract(h);
    if (!i) return kOfxStatFailed;
    Instance* inst = i->overlayOf;
    if (inst && inst->onOverlayRedraw) inst->onOverlayRedraw();
    return kOfxStatOK;
}
static const RenoVegasHWndOverlaySuiteV1 gHwndOverlaySuite = {hwoGetPropertySet, hwoRedraw};

// ====================================================================== fetchSuite
const void* fetchSuiteImpl(OfxPropertySetHandle, const char* name, int version)
{
    if (!name) return nullptr;
    Host& host = Host::get();
    const bool vegas = host.config.vegasExtensions;
    const void* suite = nullptr;
    const char* note = "";
    if (!std::strcmp(name, kOfxPropertySuite) && version == 1) suite = &gPropSuite;
    else if (!std::strcmp(name, kOfxImageEffectSuite) && version == 1) suite = &gEffectSuite;
    else if (!std::strcmp(name, kOfxParameterSuite) && version == 1) suite = &gParamSuite;
    else if (!std::strcmp(name, kOfxMemorySuite) && version == 1) suite = &gMemorySuite;
    else if (!std::strcmp(name, kOfxMultiThreadSuite) && version == 1) {
        // VEGAS's own effects get VEGAS's threading (see gVegasThreadSuite); suites are fetched while an effect loads, when the host knows which it is
        const bool own = vegas && host.config.vegasThreading && host.currentPlugin && isVegasOwnEffect(host.currentPlugin->info.id);
        suite = own ? &gVegasThreadSuite : &gThreadSuite;
        if (own) note = " (VEGAS's: one thread)";
    }
    else if (!std::strcmp(name, kOfxMessageSuite) && version == 1) suite = &gMessageSuiteV1;
    else if (!std::strcmp(name, kOfxMessageSuite) && version == 2) suite = &gMessageSuiteV2;
    else if (!std::strcmp(name, kOfxInteractSuite) && version == 1) suite = &gInteractSuite;
    else if (!std::strcmp(name, kOfxOpenGLRenderSuite) && version == 1) suite = gpuSuite();
    else if (!std::strcmp(name, kRenoProgressSuite) && version == 1) suite = &gProgressV1;
    else if (!std::strcmp(name, kRenoProgressSuite) && version == 2) suite = &gProgressV2;
    else if (!std::strcmp(name, kRenoTimeLineSuite) && version == 1) suite = &gTimeLineSuite;
    else if (vegas && !std::strcmp(name, kVegasHWndInteractSuite) && version == 1) suite = &gHwndSuite;
    else if (vegas && !std::strcmp(name, kVegasProgressSuite) && version == 1) suite = &gVegasProgressV1;
    else if (vegas && !std::strcmp(name, kVegasProgressSuite) && version == 2) suite = &gVegasProgressV2;
    else if (vegas && !std::strcmp(name, kVegasStereoscopicSuite) && version == 1) suite = &gStereoSuite;
    else if (vegas && !std::strcmp(name, kVegasKeyframeSuite) && version == 1) suite = &gKeyframeSuite;
    else if (vegas && !std::strcmp(name, kVegasEffectSuite) && version == 1) suite = &gVegasEffectSuite;
    else if (vegas && !std::strcmp(name, kVegasHWndOverlayInteractSuite) && version == 1) suite = &gHwndOverlaySuite;
    else if (vegas && host.config.stubUnknownSuites && std::strncmp(name, "OfxVegas", 8) == 0) {
        // a VEGAS suite Reno does not know at all: harmless functions that say "unsupported"
        suite = gStubSuite;
        note = " (stand-in)";
    }
    if (!suite) noteMissingSuite(name, version);
    if (host.currentPlugin) {
        std::string line = strfmt("%s v%d: %s%s", name, version, suite ? "given" : "not available", note);
        std::lock_guard<std::mutex> lk(host.currentPlugin->suiteLogMutex);
        auto& log = host.currentPlugin->suiteLog;
        if (std::find(log.begin(), log.end(), line) == log.end()) log.push_back(line);
    }
    if (host.config.verbose) logf("ofx: fetchSuite %s v%d -> %s%s", name, version, suite ? "given" : "not available", note);
    return suite;
}

}  // namespace ofx
}  // namespace reno
