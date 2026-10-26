// Property sets, parameters and parameter sets.
#include "ofxinternal.h"
#include <cfloat>
#include <climits>
#include <cmath>
#include <algorithm>

namespace reno {
namespace ofx {

int Property::dim() const
{
    switch (type) {
    case PType::Int: return (int)i.size();
    case PType::Double: return (int)d.size();
    case PType::String: return (int)s.size();
    case PType::Pointer: return (int)p.size();
    default: return 0;
    }
}

PropertySet::PropertySet(const PropertySet& o) : props_(o.props_) {}

PropertySet& PropertySet::operator=(const PropertySet& o)
{
    if (this != &o) props_ = o.props_;
    return *this;
}

Property* PropertySet::find(const std::string& name)
{
    auto it = props_.find(name);
    return it == props_.end() ? nullptr : &it->second;
}

const Property* PropertySet::find(const std::string& name) const
{
    auto it = props_.find(name);
    return it == props_.end() ? nullptr : &it->second;
}

Property& PropertySet::ensure(const std::string& name, PType t)
{
    Property& p = props_[name];
    if (p.type == PType::None) p.type = t;
    return p;
}

std::vector<std::string> PropertySet::names() const
{
    std::vector<std::string> n;
    for (const auto& kv : props_) n.push_back(kv.first);
    std::sort(n.begin(), n.end());
    return n;
}

bool PropertySet::has(const std::string& name) const { return props_.count(name) != 0; }
void PropertySet::erase(const std::string& name) { props_.erase(name); }

int PropertySet::dimension(const std::string& name) const
{
    const Property* p = find(name);
    return p ? p->dim() : 0;
}

void PropertySet::resetDim(const std::string& name, int n)
{
    Property* p = find(name);
    if (!p || n < 0) return;
    switch (p->type) {
    case PType::Int: p->i.resize((size_t)n); break;
    case PType::Double: p->d.resize((size_t)n); break;
    case PType::String: p->s.resize((size_t)n); break;
    case PType::Pointer: p->p.resize((size_t)n); break;
    default: break;
    }
}

void PropertySet::setInt(const std::string& name, int v, int idx)
{
    if (idx < 0) return;
    Property& p = ensure(name, PType::Int);
    if (p.type == PType::Double) { if ((int)p.d.size() <= idx) p.d.resize((size_t)idx + 1); p.d[(size_t)idx] = v; return; }
    if (p.type != PType::Int) return;
    if ((int)p.i.size() <= idx) p.i.resize((size_t)idx + 1);
    p.i[(size_t)idx] = v;
}

void PropertySet::setDouble(const std::string& name, double v, int idx)
{
    if (idx < 0) return;
    Property& p = ensure(name, PType::Double);
    if (p.type == PType::Int) { if ((int)p.i.size() <= idx) p.i.resize((size_t)idx + 1); p.i[(size_t)idx] = (int)std::lround(v); return; }
    if (p.type != PType::Double) return;
    if ((int)p.d.size() <= idx) p.d.resize((size_t)idx + 1);
    p.d[(size_t)idx] = v;
}

void PropertySet::setString(const std::string& name, const std::string& v, int idx)
{
    if (idx < 0) return;
    Property& p = ensure(name, PType::String);
    if (p.type != PType::String) return;
    if ((int)p.s.size() <= idx) p.s.resize((size_t)idx + 1);
    p.s[(size_t)idx] = v;
}

void PropertySet::setPointer(const std::string& name, void* v, int idx)
{
    if (idx < 0) return;
    Property& p = ensure(name, PType::Pointer);
    if (p.type != PType::Pointer) return;
    if ((int)p.p.size() <= idx) p.p.resize((size_t)idx + 1);
    p.p[(size_t)idx] = v;
}

int PropertySet::getInt(const std::string& name, int idx, int def) const
{
    const Property* p = find(name);
    if (!p || idx < 0) return def;
    if (p->type == PType::Int && idx < (int)p->i.size()) return p->i[(size_t)idx];
    if (p->type == PType::Double && idx < (int)p->d.size()) return (int)std::lround(p->d[(size_t)idx]);
    return def;
}

double PropertySet::getDouble(const std::string& name, int idx, double def) const
{
    const Property* p = find(name);
    if (!p || idx < 0) return def;
    if (p->type == PType::Double && idx < (int)p->d.size()) return p->d[(size_t)idx];
    if (p->type == PType::Int && idx < (int)p->i.size()) return p->i[(size_t)idx];
    return def;
}

std::string PropertySet::getString(const std::string& name, int idx, const std::string& def) const
{
    const Property* p = find(name);
    if (!p || idx < 0 || p->type != PType::String || idx >= (int)p->s.size()) return def;
    return p->s[(size_t)idx];
}

void* PropertySet::getPointer(const std::string& name, int idx) const
{
    const Property* p = find(name);
    if (!p || idx < 0 || p->type != PType::Pointer || idx >= (int)p->p.size()) return nullptr;
    return p->p[(size_t)idx];
}

// ------------------------------------------------------------------ parameters
static ParamKind kindOf(const std::string& t)
{
    if (t == kOfxParamTypeInteger) return ParamKind::Int;
    if (t == kOfxParamTypeDouble) return ParamKind::Double;
    if (t == kOfxParamTypeBoolean) return ParamKind::Bool;
    if (t == kOfxParamTypeChoice) return ParamKind::Choice;
    if (t == kOfxParamTypeRGBA) return ParamKind::RGBA;
    if (t == kOfxParamTypeRGB) return ParamKind::RGB;
    if (t == kOfxParamTypeDouble2D) return ParamKind::Double2D;
    if (t == kOfxParamTypeInteger2D) return ParamKind::Int2D;
    if (t == kOfxParamTypeDouble3D) return ParamKind::Double3D;
    if (t == kOfxParamTypeInteger3D) return ParamKind::Int3D;
    if (t == kOfxParamTypeString) return ParamKind::String;
    if (t == kOfxParamTypeCustom) return ParamKind::Custom;
    if (t == kOfxParamTypeGroup) return ParamKind::Group;
    if (t == kOfxParamTypePage) return ParamKind::Page;
    if (t == kOfxParamTypePushButton) return ParamKind::PushButton;
    return ParamKind::Unknown;
}

static int dimOf(ParamKind k)
{
    switch (k) {
    case ParamKind::Int: case ParamKind::Double: case ParamKind::Bool: case ParamKind::Choice: return 1;
    case ParamKind::Double2D: case ParamKind::Int2D: return 2;
    case ParamKind::Double3D: case ParamKind::Int3D: case ParamKind::RGB: return 3;
    case ParamKind::RGBA: return 4;
    default: return 0;
    }
}

std::string Param::label() const
{
    std::string l = props.getString(kOfxPropLabel);
    return l.empty() ? name : l;
}

std::vector<std::string> Param::choices() const
{
    std::vector<std::string> out;
    const Property* p = props.find(kOfxParamPropChoiceOption);
    if (p && p->type == PType::String) out = p->s;
    return out;
}

std::vector<double> Param::valueAt(double time) const
{
    if (keys.empty() || dim == 0) return value;
    auto hi = keys.lower_bound(time);
    if (hi == keys.end()) return std::prev(hi)->second;
    if (hi == keys.begin() || hi->first == time) return hi->second;
    auto lo = std::prev(hi);
    if (kind == ParamKind::Bool || kind == ParamKind::Choice) return lo->second;
    double t = (time - lo->first) / (hi->first - lo->first);
    switch (keyCurve(lo->first)) {
    case KeyFast: t = 1.0 - (1.0 - t) * (1.0 - t); break;
    case KeySlow: t = t * t; break;
    case KeySmooth: t = t * t * (3.0 - 2.0 * t); break;
    case KeySharp: t = 0.5 - std::sin(std::asin(std::max(-1.0, std::min(1.0, 1.0 - 2.0 * t))) / 3.0); break;
    case KeyHold: return lo->second;
    default: break;
    }
    std::vector<double> v((size_t)dim);
    for (int k = 0; k < dim; ++k) {
        double a = k < (int)lo->second.size() ? lo->second[(size_t)k] : 0.0;
        double b = k < (int)hi->second.size() ? hi->second[(size_t)k] : 0.0;
        v[(size_t)k] = a + (b - a) * t;
        if (isWhole()) v[(size_t)k] = std::floor(v[(size_t)k] + 0.5);
    }
    return v;
}

std::string Param::strAt(double time) const
{
    if (strKeys.empty()) return str;
    auto hi = strKeys.upper_bound(time);
    if (hi == strKeys.begin()) return hi->second;
    return std::prev(hi)->second;
}

Param* ParamSet::find(const std::string& name) const
{
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : it->second;
}

// The properties each kind of parameter starts with: default, range and so on.
static void setKindDefaults(Param& p)
{
    PropertySet& s = p.props;
    switch (p.kind) {
    case ParamKind::Int: case ParamKind::Int2D: case ParamKind::Int3D:
        for (int k = 0; k < p.dim; ++k) {
            s.setInt(kOfxParamPropDefault, 0, k);
            s.setInt(kOfxParamPropMin, INT_MIN, k);
            s.setInt(kOfxParamPropMax, INT_MAX, k);
            s.setInt(kOfxParamPropDisplayMin, INT_MIN, k);
            s.setInt(kOfxParamPropDisplayMax, INT_MAX, k);
        }
        break;
    case ParamKind::Double: case ParamKind::Double2D: case ParamKind::Double3D:
        for (int k = 0; k < p.dim; ++k) {
            s.setDouble(kOfxParamPropDefault, 0, k);
            s.setDouble(kOfxParamPropMin, -DBL_MAX, k);
            s.setDouble(kOfxParamPropMax, DBL_MAX, k);
            s.setDouble(kOfxParamPropDisplayMin, -DBL_MAX, k);
            s.setDouble(kOfxParamPropDisplayMax, DBL_MAX, k);
        }
        s.setDouble(kOfxParamPropIncrement, 1.0);
        s.setInt(kOfxParamPropDigits, 2);
        s.setString(kOfxParamPropDoubleType, kOfxParamDoubleTypePlain);
        s.setString(kOfxParamPropDefaultCoordinateSystem, kOfxParamCoordinatesCanonical);
        s.setInt(kOfxParamPropShowTimeMarker, 0);
        break;
    case ParamKind::RGB: case ParamKind::RGBA:
        for (int k = 0; k < p.dim; ++k) {
            s.setDouble(kOfxParamPropDefault, k == 3 ? 1.0 : 0.0, k);
            s.setDouble(kOfxParamPropMin, 0.0, k);
            s.setDouble(kOfxParamPropMax, 1.0, k);
            s.setDouble(kOfxParamPropDisplayMin, 0.0, k);
            s.setDouble(kOfxParamPropDisplayMax, 1.0, k);
        }
        break;
    case ParamKind::Bool:
        s.setInt(kOfxParamPropDefault, 0);
        break;
    case ParamKind::Choice:
        s.setInt(kOfxParamPropDefault, 0);
        s.ensure(kOfxParamPropChoiceOption, PType::String);
        break;
    case ParamKind::String:
        s.setString(kOfxParamPropDefault, "");
        s.setString(kOfxParamPropStringMode, kOfxParamStringIsSingleLine);
        s.setInt(kOfxParamPropStringFilePathExists, 1);
        break;
    case ParamKind::Custom:
        s.setString(kOfxParamPropDefault, "");
        s.setPointer(kOfxParamPropCustomInterpCallbackV1, nullptr);
        break;
    case ParamKind::Group:
        s.setInt(kOfxParamPropGroupOpen, 1);
        break;
    case ParamKind::Page:
        s.ensure(kOfxParamPropPageChild, PType::String);
        break;
    default:
        break;
    }
}

Param* ParamSet::define(const std::string& type, const std::string& name)
{
    if (find(name)) return nullptr;
    // VEGAS's own "image" parameter (Color Match's ImageSource): kept, with no value of its own - VEGAS's parameter
    // suite answers kOfxStatErrUnsupported for its value too - and no row in the effects window.
    const bool vegasImage = type == kVegasParamTypeImage && Host::get().config.vegasExtensions;
    if (kindOf(type) == ParamKind::Unknown && !vegasImage) return nullptr;
    std::unique_ptr<Param> up(new Param());
    Param* p = up.get();
    p->set = this;
    p->name = name;
    p->type = type;
    p->kind = kindOf(type);
    p->dim = dimOf(p->kind);
    PropertySet& s = p->props;
    s.setString(kOfxPropType, kOfxTypeParameter);
    s.setString(kOfxPropName, name);
    s.setString(kOfxPropLabel, name);
    s.setString(kOfxPropShortLabel, name);
    s.setString(kOfxPropLongLabel, name);
    s.setString(kOfxParamPropType, type);
    s.setInt(kOfxParamPropSecret, 0);
    s.setString(kOfxParamPropHint, "");
    s.setString(kOfxParamPropScriptName, name);
    s.setString(kOfxParamPropParent, "");
    s.setInt(kOfxParamPropEnabled, 1);
    s.setPointer(kOfxParamPropDataPtr, nullptr);
    s.setString(kOfxPropIcon, "", 0);
    s.setString(kOfxPropIcon, "", 1);
    s.setPointer(kOfxParamPropInteractV1, nullptr);
    s.setDouble(kOfxParamPropInteractSize, 0, 0);
    s.setDouble(kOfxParamPropInteractSize, 0, 1);
    s.setDouble(kOfxParamPropInteractSizeAspect, 1.0);
    s.setDouble(kOfxParamPropInteractMinimumSize, 10, 0);
    s.setDouble(kOfxParamPropInteractMinimumSize, 10, 1);
    s.setInt(kOfxParamPropInteractPreferedSize, 10, 0);
    s.setInt(kOfxParamPropInteractPreferedSize, 10, 1);
    if (vegasImage) {
        s.setInt(kVegasParamImageHideGrabFromPreview, 0);
        s.setInt(kVegasParamImageHideGrabControls, 0);
    }
    if (p->hasValue()) {
        const bool numeric = p->dim > 0;
        const bool canAnimate = numeric && p->kind != ParamKind::Bool && p->kind != ParamKind::Choice;
        s.setInt(kOfxParamPropAnimates, canAnimate ? 1 : 0);
        s.setInt(kOfxParamPropIsAnimating, 0);
        s.setInt(kOfxParamPropIsAutoKeying, 0);
        s.setInt(kOfxParamPropPersistant, 1);
        s.setInt(kOfxParamPropEvaluateOnChange, 1);
        s.setInt(kOfxParamPropPluginMayWrite, 0);
        s.setString(kOfxParamPropCacheInvalidation, kOfxParamInvalidateValueChange);
        s.setInt(kOfxParamPropCanUndo, 1);
    }
    setKindDefaults(*p);
    if (p->dim > 0) {
        static const char* const xyz[] = {"x", "y", "z"};
        static const char* const rgba[] = {"r", "g", "b", "a"};
        const bool colour = p->kind == ParamKind::RGB || p->kind == ParamKind::RGBA;
        if (p->dim > 1)
            for (int k = 0; k < p->dim; ++k) s.setString(kOfxParamPropDimensionLabel, colour ? rgba[k] : xyz[k], k);
    }
    p->value.assign((size_t)p->dim, 0.0);
    byName_[name] = p;
    params.push_back(std::move(up));
    return p;
}

void ParamSet::cloneFrom(const ParamSet& o)
{
    params.clear();
    byName_.clear();
    props = o.props;
    for (const auto& src : o.params) {
        std::unique_ptr<Param> up(new Param());
        Param* p = up.get();
        p->set = this;
        p->name = src->name;
        p->type = src->type;
        p->kind = src->kind;
        p->dim = src->dim;
        p->props = src->props;
        p->value.assign((size_t)p->dim, 0.0);
        for (int k = 0; k < p->dim; ++k) p->value[(size_t)k] = p->props.getDouble(kOfxParamPropDefault, k, 0.0);
        if (p->kind == ParamKind::String || p->kind == ParamKind::Custom) p->str = p->props.getString(kOfxParamPropDefault);
        byName_[p->name] = p;
        params.push_back(std::move(up));
    }
}

ClipDesc* Descriptor::findClip(const std::string& name) const
{
    for (const auto& c : clips) if (c->name == name) return c.get();
    return nullptr;
}

const char* statusName(OfxStatus s)
{
    switch (s) {
    case kOfxStatOK: return "OK";
    case kOfxStatFailed: return "failed";
    case kOfxStatErrFatal: return "fatal error";
    case kOfxStatErrUnknown: return "unknown";
    case kOfxStatErrMissingHostFeature: return "missing host feature";
    case kOfxStatErrUnsupported: return "unsupported";
    case kOfxStatErrExists: return "already exists";
    case kOfxStatErrFormat: return "bad format";
    case kOfxStatErrMemory: return "out of memory";
    case kOfxStatErrBadHandle: return "bad handle";
    case kOfxStatErrBadIndex: return "bad index";
    case kOfxStatErrValue: return "bad value";
    case kOfxStatReplyYes: return "yes";
    case kOfxStatReplyNo: return "no";
    case kOfxStatReplyDefault: return "default";
    case kOfxStatUnlicensed: return "unlicensed";
    case kOfxStatErrImageFormat: return "bad image format";
    default: return "?";
    }
}

// ====================================================================== what a plug-in found missing
MissingTrace& missingTrace()
{
    static thread_local MissingTrace t;
    return t;
}

std::string propSetKind(const PropertySet* s)
{
    if (!s) return "no property set";
    if (s == &Host::get().hostProps) return "the host's properties";
    const Property* t = s->find(kOfxPropType);
    const std::string type = (t && t->type == PType::String && !t->s.empty()) ? t->s[0] : std::string();
    if (type == kOfxTypeImageEffectHost) return "the host's properties";
    if (type == kOfxTypeImageEffect) return "the effect's descriptor";
    if (type == kOfxTypeImageEffectInstance) return "the effect instance";
    if (type == kOfxTypeClip) return "a clip";
    if (type == kOfxTypeImage) return "an image";
    if (type == kOfxTypeParameter || type == kOfxTypeParameterInstance) return "a parameter";
    if (!type.empty()) return type;
    return "the action's arguments";
}

static void addMissing(Plugin* pl, const std::string& line)
{
    if (!pl) return;
    std::lock_guard<std::mutex> lk(pl->suiteLogMutex);
    auto& log = pl->missingLog;
    if (log.size() < 400 && std::find(log.begin(), log.end(), line) == log.end()) log.push_back(line);
}

OfxStatus noteMissingProp(PropertySet* s, const char* name, int index, OfxStatus st)
{
    MissingTrace& t = missingTrace();
    Plugin* pl = t.plugin ? t.plugin : Host::currentPlugin;
    if (!pl && !Host::get().config.verbose) return st;
    std::string line = std::string(name ? name : "?");
    if (st == kOfxStatErrBadIndex && index >= 0) line += strfmt("[%d] (only %d values)", index, s ? s->dimension(name ? name : "") : 0);
    line += " (" + propSetKind(s) + ")";
    t.prop = line;
    addMissing(pl, line);
    if (Host::get().config.verbose) logf("ofx: %s asked for property %s: not there", pl ? pl->info.id.c_str() : "?", line.c_str());
    return st;
}

void noteMissingSuite(const char* name, int version)
{
    MissingTrace& t = missingTrace();
    Plugin* pl = t.plugin ? t.plugin : Host::currentPlugin;
    t.suite = strfmt("%s v%d", name ? name : "?", version);
    (void)pl;
}

std::string Plugin::explainStatus(OfxStatus st) const
{
    std::string s = statusName(st);
    if (st == kOfxStatErrMissingHostFeature && !missingFeature.empty()) s += ": " + missingFeature;
    return s;
}

}  // namespace ofx
}  // namespace reno
