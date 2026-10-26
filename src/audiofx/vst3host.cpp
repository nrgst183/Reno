// VST3 effect host.
//
// The VST3 interfaces are declared in vst3_abi.h (our own declarations, checked against the official headers; no
// Steinberg SDK is needed to build).
//
// What a plug-in is asked to do, in VST3's order:
//   the file      load it, InitDll (Windows) / ModuleEntry (Linux), GetPluginFactory. Once per file; never unloaded.
//   an instance   createInstance of the component, initialize; the controller is the same object or a second one
//                 made from the class id the component names; the two are connected; the controller is given the
//                 component's state; the main audio buses are asked to be stereo and switched on, other audio buses
//                 are switched off.
//   activate()    setupProcessing, setActive(true), setProcessing(true)
//   process()     process, in pieces of at most the block size given to activate()
//   deactivate()  setProcessing(false), setActive(false)
//   the end       the reverse: window closed, deactivate, disconnect, terminate, release.
//
// Threads: process() runs on the audio thread, everything else on the thread that owns the plug-in (the UI
// thread). A parameter changed on the UI thread is told to the controller there and queued; process() hands it to
// the processor with the next block.
#include "audioplugin.h"
#include "../core/dynlib.h"
#include "vst3_abi.h"
#include <cstring>
#include <cmath>
#include <mutex>
#include <atomic>
#include <map>
#include <unordered_map>
#include <algorithm>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace reno {

namespace {

using namespace vst3;

// ------------------------------------------------------------------ what differs between platforms
#if defined(_WIN32)
const FIDString kWindowType = kPlatformTypeHWND;                    // openEditor's parent is an HWND
#  if defined(_M_ARM64)
const char* const kBundleArch = "arm64-win";
#  elif defined(_WIN64)
const char* const kBundleArch = "x86_64-win";
#  else
const char* const kBundleArch = "x86-win";
#  endif
#else
const FIDString kWindowType = kPlatformTypeX11EmbedWindowID;        // openEditor's parent is an X11 window id
#  if defined(__aarch64__)
const char* const kBundleArch = "aarch64-linux";
#  elif defined(__x86_64__)
const char* const kBundleArch = "x86_64-linux";
#  else
const char* const kBundleArch = "i386-linux";
#  endif
#endif

// ------------------------------------------------------------------ small helpers
template <class T> void drop(T*& p)
{
    if (p) p->release();
    p = nullptr;
}

// Asks 'unknown' for another interface; null if absent. The caller releases it.
template <class T> T* queryFor(FUnknown* unknown, const TUID iid)
{
    void* obj = nullptr;
    if (!unknown || unknown->queryInterface(iid, &obj) != kResultOk) return nullptr;
    return static_cast<T*>(obj);
}

void appendUtf8(std::string& out, uint32_t c)
{
    if (c < 0x80) out.push_back((char)c);
    else if (c < 0x800) { out.push_back((char)(0xC0 | (c >> 6))); out.push_back((char)(0x80 | (c & 0x3F))); }
    else if (c < 0x10000) {
        out.push_back((char)(0xE0 | (c >> 12)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (c >> 18)));
        out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    }
}

// UTF-16 text in a field of 'maxUnits' units (may be full, with no zero at the end) to UTF-8.
std::string fromUtf16(const char16_t* s, size_t maxUnits)
{
    std::string out;
    if (!s) return out;
    for (size_t i = 0; i < maxUnits && s[i]; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < maxUnits && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((uint32_t)s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;                             // half of a pair on its own
        }
        appendUtf8(out, c);
    }
    return out;
}

// 8-bit text in a field of 'size' bytes (may be full, with no zero at the end).
std::string fromField(const char* s, size_t size)
{
    size_t n = 0;
    while (n < size && s[n]) ++n;
    return std::string(s, n);
}

// A class id as text: 32 hexadecimal digits, the same on every platform (the four 32-bit numbers as the plug-in
// author wrote them, most significant digit first, as the SDK and other hosts print).
std::string uidToText(const TUID id)
{
    unsigned char b[16];
    std::memcpy(b, id, 16);
#if RENO_VST3_COM_STYLE
    std::swap(b[0], b[3]); std::swap(b[1], b[2]); std::swap(b[4], b[5]); std::swap(b[6], b[7]);
#endif
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (int k = 0; k < 16; ++k) { out.push_back(kHex[b[k] >> 4]); out.push_back(kHex[b[k] & 15]); }
    return out;
}

bool uidFromText(const std::string& text, TUID id)
{
    unsigned char b[16];
    int digits = 0;
    for (char ch : text) {
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else if (ch == '-' || ch == '{' || ch == '}' || ch == ' ') continue;
        else return false;
        if (digits >= 32) return false;
        if (digits & 1) b[digits / 2] = (unsigned char)(b[digits / 2] | v);
        else b[digits / 2] = (unsigned char)(v << 4);
        ++digits;
    }
    if (digits != 32) return false;
#if RENO_VST3_COM_STYLE
    std::swap(b[0], b[3]); std::swap(b[1], b[2]); std::swap(b[4], b[5]); std::swap(b[6], b[7]);
#endif
    std::memcpy(id, b, 16);
    return true;
}

// ------------------------------------------------------------------ objects of ours that plug-ins hold
// Reference counting for an object that lives as long as somebody holds it.
template <class Self, class Interface> class Counted : public Interface {
public:
    uint32_t PLUGIN_API addRef() override { return ++refs_; }
    uint32_t PLUGIN_API release() override
    {
        const uint32_t left = --refs_;
        if (left == 0) delete static_cast<Self*>(this);
        return left;
    }

protected:
    Counted() : refs_(1) {}
    ~Counted() {}

private:
    std::atomic<uint32_t> refs_;
};

// Reference counting for an object that is part of something else and goes with it.
template <class Interface> class Uncounted : public Interface {
public:
    uint32_t PLUGIN_API addRef() override { return 2; }
    uint32_t PLUGIN_API release() override { return 1; }
};

// queryInterface for an object with one interface.
template <class Interface> tresult queryOne(Interface* self, const TUID wanted, const TUID own, void** obj)
{
    if (!obj) return kInvalidArgument;
    if (iidEqual(wanted, FUnknown_iid) || iidEqual(wanted, own)) {
        self->addRef();
        *obj = self;
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

// A stream over a block of memory (a plug-in's state on its way in or out).
class MemStream final : public Counted<MemStream, IBStream> {
public:
    std::string data;
    size_t pos = 0;

    MemStream() {}
    explicit MemStream(std::string bytes) : data(std::move(bytes)) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IBStream>(this, iid, IBStream_iid, obj); }

    tresult PLUGIN_API read(void* buffer, int32_t numBytes, int32_t* numBytesRead) override
    {
        if (numBytesRead) *numBytesRead = 0;
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const size_t left = pos < data.size() ? data.size() - pos : 0;
        const size_t n = std::min(left, (size_t)numBytes);
        if (n) std::memcpy(buffer, data.data() + pos, n);
        pos += n;
        if (numBytesRead) *numBytesRead = (int32_t)n;
        // a caller that does not ask how much it got can only be told about a short read this way
        return (numBytesRead || n == (size_t)numBytes) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API write(void* buffer, int32_t numBytes, int32_t* numBytesWritten) override
    {
        if (numBytesWritten) *numBytesWritten = 0;
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const size_t n = (size_t)numBytes;
        if (pos > data.size()) pos = data.size();
        if (pos + n > data.size()) data.resize(pos + n);
        if (n) std::memcpy(&data[pos], buffer, n);
        pos += n;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64_t where, int32_t mode, int64_t* result) override
    {
        int64_t from;
        if (mode == kIBSeekSet) from = 0;
        else if (mode == kIBSeekCur) from = (int64_t)pos;
        else if (mode == kIBSeekEnd) from = (int64_t)data.size();
        else return kInvalidArgument;
        int64_t to = from + where;
        if (to < 0) to = 0;
        if (to > (int64_t)data.size()) to = (int64_t)data.size();
        pos = (size_t)to;
        if (result) *result = to;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64_t* where) override
    {
        if (!where) return kInvalidArgument;
        *where = (int64_t)pos;
        return kResultOk;
    }
};

// The named values of a message one half of a plug-in sends the other.
class HostAttributeList final : public Counted<HostAttributeList, IAttributeList> {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IAttributeList>(this, iid, IAttributeList_iid, obj); }

    tresult PLUGIN_API setInt(AttrID id, int64_t value) override
    {
        if (!id) return kInvalidArgument;
        Item& it = items_[id];
        it = Item();
        it.kind = kInt;
        it.i = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64_t& value) override
    {
        const Item* it = find(id, kInt);
        if (!it) return kResultFalse;
        value = it->i;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override
    {
        if (!id) return kInvalidArgument;
        Item& it = items_[id];
        it = Item();
        it.kind = kFloat;
        it.f = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override
    {
        const Item* it = find(id, kFloat);
        if (!it) return kResultFalse;
        value = it->f;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const TChar* string) override
    {
        if (!id || !string) return kInvalidArgument;
        Item& it = items_[id];
        it = Item();
        it.kind = kString;
        it.s = string;
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, TChar* string, uint32_t sizeInBytes) override
    {
        const Item* it = find(id, kString);
        if (!it || !string) return kResultFalse;
        const size_t room = sizeInBytes / sizeof(TChar);
        if (room == 0) return kResultOk;
        const size_t n = std::min(it->s.size(), room - 1);
        std::memcpy(string, it->s.data(), n * sizeof(TChar));
        string[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32_t sizeInBytes) override
    {
        if (!id || (!data && sizeInBytes)) return kInvalidArgument;
        Item& it = items_[id];
        it = Item();
        it.kind = kBinary;
        if (sizeInBytes) it.bytes.assign((const char*)data, sizeInBytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32_t& sizeInBytes) override
    {
        const Item* it = find(id, kBinary);
        if (!it) { data = nullptr; sizeInBytes = 0; return kResultFalse; }
        data = it->bytes.data();
        sizeInBytes = (uint32_t)it->bytes.size();
        return kResultOk;
    }

private:
    enum Kind { kNone, kInt, kFloat, kString, kBinary };
    struct Item {
        Kind kind = kNone;
        int64_t i = 0;
        double f = 0.0;
        std::u16string s;
        std::string bytes;
    };
    std::map<std::string, Item> items_;

    const Item* find(AttrID id, Kind kind) const
    {
        if (!id) return nullptr;
        auto it = items_.find(id);
        return (it != items_.end() && it->second.kind == kind) ? &it->second : nullptr;
    }
};

class HostMessage final : public Counted<HostMessage, IMessage> {
public:
    HostMessage() : attributes_(new HostAttributeList()) {}
    ~HostMessage() { attributes_->release(); }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IMessage>(this, iid, IMessage_iid, obj); }
    FIDString PLUGIN_API getMessageID() override { return hasId_ ? id_.c_str() : nullptr; }
    void PLUGIN_API setMessageID(FIDString id) override
    {
        hasId_ = id != nullptr;
        id_ = id ? id : "";
    }
    IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::string id_;
    bool hasId_ = false;
    HostAttributeList* attributes_;
};

// What every plug-in is given at start: who the host is, a way to make messages, and which of its interfaces the
// host uses.
class HostApp final : public IHostApplication, public IPlugInterfaceSupport {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        if (iidEqual(iid, FUnknown_iid) || iidEqual(iid, IHostApplication_iid)) *obj = static_cast<IHostApplication*>(this);
        else if (iidEqual(iid, IPlugInterfaceSupport_iid)) *obj = static_cast<IPlugInterfaceSupport*>(this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        return kResultOk;
    }
    uint32_t PLUGIN_API addRef() override { return 2; }         // never freed
    uint32_t PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getName(String128 name) override
    {
        if (!name) return kInvalidArgument;
        const char16_t text[] = u"Reno";
        std::memcpy(name, text, sizeof(text));
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (!cid || !iid) return kInvalidArgument;
        if (iidEqual(cid, IMessage_iid) && iidEqual(iid, IMessage_iid)) {
            *obj = static_cast<IMessage*>(new HostMessage());
            return kResultOk;
        }
        if (iidEqual(cid, IAttributeList_iid) && iidEqual(iid, IAttributeList_iid)) {
            *obj = static_cast<IAttributeList*>(new HostAttributeList());
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API isPlugInterfaceSupported(const TUID iid) override
    {
        if (!iid) return kInvalidArgument;
        const bool used = iidEqual(iid, IComponent_iid) || iidEqual(iid, IAudioProcessor_iid) || iidEqual(iid, IEditController_iid) ||
                          iidEqual(iid, IConnectionPoint_iid);
        return used ? kResultTrue : kResultFalse;
    }
};

FUnknown* hostApp()
{
    static HostApp* app = new HostApp();        // never deleted: plug-ins may still hold it at exit
    return static_cast<IHostApplication*>(app);
}

// True on the audio thread while a plug-in is inside process().
thread_local bool tInProcess = false;

// On Windows a plug-in that crashes while processing or opening its window is stopped there and not asked to
// process again, instead of taking the program down, as VEGAS does; VST2 plug-ins and OpenFX effects are guarded
// the same way.
#ifdef _WIN32
tresult guardedProcess(IAudioProcessor* processor, ProcessData& data, unsigned long* code)
{
    __try {
        return processor->process(data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return kResultFalse;
    }
}
tresult guardedAttach(IPlugView* view, void* parent, FIDString type, unsigned long* code)
{
    __try {
        return view->attached(parent, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return kResultFalse;
    }
}
#endif

// Sits between the two halves of a plug-in: each half is connected to one of these instead of to the other half, and
// messages are passed on. Neither half can reach the other except by messages (as VST3 recommends), and nothing is
// passed on from inside process(), where sending messages is not allowed and the controller is not ready.
class Link final : public Counted<Link, IConnectionPoint> {
public:
    explicit Link(IConnectionPoint* target) : target_(target) { target->addRef(); }
    ~Link() { cut(); }
    void cut()
    {
        IConnectionPoint* t = target_.exchange(nullptr);
        if (t) t->release();
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IConnectionPoint>(this, iid, IConnectionPoint_iid, obj); }
    tresult PLUGIN_API connect(IConnectionPoint*) override { return kResultFalse; }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { return kResultFalse; }
    tresult PLUGIN_API notify(IMessage* message) override
    {
        IConnectionPoint* t = target_;
        if (!t || !message || tInProcess) return kResultFalse;
        return t->notify(message);
    }

private:
    std::atomic<IConnectionPoint*> target_;
};

// The values one parameter takes during a block.
class ParamQueue final : public Uncounted<IParamValueQueue> {
public:
    struct Point {
        int32_t offset;
        ParamValue value;
    };
    ParamID id = 0;
    std::vector<Point> points;

    ParamQueue() { points.reserve(16); }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IParamValueQueue>(this, iid, IParamValueQueue_iid, obj); }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32_t PLUGIN_API getPointCount() override { return (int32_t)points.size(); }

    tresult PLUGIN_API getPoint(int32_t index, int32_t& sampleOffset, ParamValue& value) override
    {
        if (index < 0 || index >= (int32_t)points.size()) return kResultFalse;
        sampleOffset = points[(size_t)index].offset;
        value = points[(size_t)index].value;
        return kResultOk;
    }

    tresult PLUGIN_API addPoint(int32_t sampleOffset, ParamValue value, int32_t& index) override
    {
        size_t at = points.size();                  // kept in order of time
        while (at > 0 && points[at - 1].offset > sampleOffset) --at;
        if (at > 0 && points[at - 1].offset == sampleOffset) {
            points[at - 1].value = value;
            index = (int32_t)(at - 1);
            return kResultOk;
        }
        Point p;
        p.offset = sampleOffset;
        p.value = value;
        points.insert(points.begin() + (std::ptrdiff_t)at, p);
        index = (int32_t)at;
        return kResultOk;
    }
};

// The parameters that change during a block, one queue each.
class ParamChanges final : public Uncounted<IParameterChanges> {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IParameterChanges>(this, iid, IParameterChanges_iid, obj); }
    int32_t PLUGIN_API getParameterCount() override { return used_; }

    IParamValueQueue* PLUGIN_API getParameterData(int32_t index) override { return queue(index); }

    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32_t& index) override
    {
        for (int32_t k = 0; k < used_; ++k)
            if (queues_[(size_t)k]->id == id) { index = k; return queues_[(size_t)k].get(); }
        if ((size_t)used_ >= queues_.size()) queues_.emplace_back(new ParamQueue());     // rare: room is made in reserve()
        ParamQueue* q = queues_[(size_t)used_].get();
        q->id = id;
        q->points.clear();
        index = used_++;
        return q;
    }

    ParamQueue* queue(int32_t index) { return (index >= 0 && index < used_) ? queues_[(size_t)index].get() : nullptr; }
    void clear() { used_ = 0; }
    void reserve(size_t n)
    {
        while (queues_.size() < n) queues_.emplace_back(new ParamQueue());
    }

private:
    std::vector<std::unique_ptr<ParamQueue>> queues_;
    int32_t used_ = 0;
};

// Notes and the like: Reno sends none and keeps none.
class NoEvents final : public Uncounted<IEventList> {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IEventList>(this, iid, IEventList_iid, obj); }
    int32_t PLUGIN_API getEventCount() override { return 0; }
    tresult PLUGIN_API getEvent(int32_t, Event&) override { return kResultFalse; }
    tresult PLUGIN_API addEvent(Event&) override { return kResultFalse; }
};

// ------------------------------------------------------------------ a loaded plug-in file
struct Module {
    struct Class {
        TUID cid;
        std::string category, name, vendor, subCategories;
    };
    DynLib lib;
    IPluginFactory* factory = nullptr;
    std::string vendor;
    std::vector<Class> classes;

    const Class* find(const TUID cid) const
    {
        for (const auto& c : classes)
            if (iidEqual(c.cid, cid)) return &c;
        return nullptr;
    }
};

// 'path' is normally the file; a bundle folder (Name.vst3/Contents/<architecture>/...) is looked into.
std::string moduleFile(const std::string& path)
{
    if (!dirExists(path)) return path;
    const std::string bin = pathJoin(pathJoin(path, "Contents"), kBundleArch);
    for (const auto& f : listDir(bin))
        if (endsWithNoCase(f, ".vst3") || endsWithNoCase(f, ".so")) return pathJoin(bin, f);
    return path;
}

void readClasses(Module& mod)
{
    IPluginFactory* factory = mod.factory;
    IPluginFactory2* factory2 = queryFor<IPluginFactory2>(factory, IPluginFactory2_iid);
    IPluginFactory3* factory3 = queryFor<IPluginFactory3>(factory, IPluginFactory3_iid);
    if (factory3) factory3->setHostContext(hostApp());

    PFactoryInfo fi;
    std::memset(&fi, 0, sizeof(fi));
    if (factory->getFactoryInfo(&fi) == kResultOk) mod.vendor = fromField(fi.vendor, sizeof(fi.vendor));

    int32_t count = factory->countClasses();
    if (count < 0) count = 0;
    if (count > 4096) count = 4096;
    for (int32_t k = 0; k < count; ++k) {
        Module::Class c;
        bool got = false;
        if (factory3) {                             // newest description first: its texts are UTF-16
            PClassInfoW w;
            std::memset(&w, 0, sizeof(w));
            if (factory3->getClassInfoUnicode(k, &w) == kResultOk) {
                std::memcpy(c.cid, w.cid, sizeof(TUID));
                c.category = fromField(w.category, sizeof(w.category));
                c.name = fromUtf16(w.name, sizeof(w.name) / sizeof(w.name[0]));
                c.vendor = fromUtf16(w.vendor, sizeof(w.vendor) / sizeof(w.vendor[0]));
                c.subCategories = fromField(w.subCategories, sizeof(w.subCategories));
                got = true;
            }
        }
        if (!got && factory2) {
            PClassInfo2 i2;
            std::memset(&i2, 0, sizeof(i2));
            if (factory2->getClassInfo2(k, &i2) == kResultOk) {
                std::memcpy(c.cid, i2.cid, sizeof(TUID));
                c.category = fromField(i2.category, sizeof(i2.category));
                c.name = fromField(i2.name, sizeof(i2.name));
                c.vendor = fromField(i2.vendor, sizeof(i2.vendor));
                c.subCategories = fromField(i2.subCategories, sizeof(i2.subCategories));
                got = true;
            }
        }
        if (!got) {
            PClassInfo i1;
            std::memset(&i1, 0, sizeof(i1));
            if (factory->getClassInfo(k, &i1) == kResultOk) {
                std::memcpy(c.cid, i1.cid, sizeof(TUID));
                c.category = fromField(i1.category, sizeof(i1.category));
                c.name = fromField(i1.name, sizeof(i1.name));
                got = true;
            }
        }
        if (got) mod.classes.push_back(c);
    }
    drop(factory3);
    drop(factory2);
}

// Loads a plug-in file, or finds it if already loaded. A loaded file stays loaded (so does its factory):
// unloading a plug-in that has had a window open is unsafe, and the start-up function must run only once however
// many effects are made from it.
Module* loadModule(const std::string& path, std::string* error)
{
    static std::mutex mutex;
    static std::map<std::string, Module*>* loaded = new std::map<std::string, Module*>();
    std::lock_guard<std::mutex> lk(mutex);

    const std::string file = moduleFile(path);
#ifdef _WIN32
    std::string key = toLower(file);
    for (auto& ch : key) if (ch == '/') ch = '\\';
#else
    const std::string key = file;
#endif
    auto found = loaded->find(key);
    if (found != loaded->end()) return found->second;

    std::unique_ptr<Module> mod(new Module());
    std::string e;
    if (!mod->lib.open(file, &e)) {
        if (error) *error = "could not be loaded (" + e + ")";
        return nullptr;
    }
    GetFactoryProc getFactory = (GetFactoryProc)mod->lib.symbol("GetPluginFactory");
    if (!getFactory) {
        if (error) *error = "is not a VST3 plug-in";
        return nullptr;                             // nothing in it has run: unloaded again
    }
#ifdef _WIN32
    InitDllProc start = (InitDllProc)mod->lib.symbol("InitDll");
    const bool started = !start || start();
#else
    ModuleEntryProc start = (ModuleEntryProc)mod->lib.symbol("ModuleEntry");
    const bool started = !start || start(mod->lib.nativeHandle());
#endif
    mod->lib.leak();                                // from here on the file is never unloaded
    if (!started) {
        if (error) *error = "did not start (its start-up function failed)";
        return nullptr;
    }
    mod->factory = getFactory();
    if (!mod->factory) {
        if (error) *error = "did not start (it gave no plug-in factory)";
        return nullptr;
    }
    readClasses(*mod);
    Module* result = mod.release();
    (*loaded)[key] = result;
    return result;
}

// ------------------------------------------------------------------ what a plug-in calls back
class Vst3Plugin;

// Given to the controller: the plug-in's window reports what the user does in it here.
class Handler final : public Counted<Handler, IComponentHandler> {
public:
    explicit Handler(Vst3Plugin* owner) : owner_(owner) {}
    void forgetOwner() { owner_ = nullptr; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IComponentHandler>(this, iid, IComponentHandler_iid, obj); }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override;
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32_t flags) override;

private:
    std::atomic<Vst3Plugin*> owner_;
};

// Given to the plug-in's window: it asks for a different size here.
class Frame final : public Counted<Frame, IPlugFrame> {
public:
    explicit Frame(Vst3Plugin* owner) : owner_(owner) {}
    void forgetOwner() { owner_ = nullptr; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override { return queryOne<IPlugFrame>(this, iid, IPlugFrame_iid, obj); }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override;

private:
    std::atomic<Vst3Plugin*> owner_;
};

// ------------------------------------------------------------------ one effect
class Vst3Plugin final : public AudioPlugin {
public:
    Module* module = nullptr;
    IComponent* component = nullptr;                // the half that does the audio
    IAudioProcessor* processor = nullptr;           // the same object
    std::atomic<bool> crashed{false};               // (Windows) crashed while processing: never asked again
    IEditController* controller = nullptr;          // the half that knows the parameters and has the window
    IConnectionPoint* componentLink = nullptr;      // the two halves' ends of the connection between them ...
    IConnectionPoint* controllerLink = nullptr;
    Link* componentSees = nullptr;                  // ... and what each is connected to (passes messages on)
    Link* controllerSees = nullptr;
    bool componentStarted = false, controllerStarted = false, controllerIsComponent = false, linked = false;
    Handler* handler = nullptr;
    Frame* frame = nullptr;
    IPlugView* view = nullptr;                      // while the window is open
    bool viewAttached = false, resizing = false;
    int editorKnown = -1;                           // -1 not asked yet, 0 none, 1 has one

    // audio (all of it under 'mutex')
    std::mutex mutex;                               // process() against activate()/deactivate()/loadState()
    bool active = false;
    double sampleRate = 48000.0;
    int blockSize = 1024;
    int64_t blocksDone = 0;                         // samples handed to the plug-in since activate()
    std::atomic<int> latency{0};                    // what the plug-in said last
    std::atomic<bool> latencyKnown{false};          // false: ask again (never asked, or the plug-in said it changed)
    std::vector<int> inChannels, outChannels;       // channels of each audio bus
    std::vector<float> inBuf[6], outBuf[6];          // our channels of the main buses: 2, or 6 (setChannels)
    int chans = 2;                                  // what the main buses carry: 2 (stereo) or 6 (5.1)
    std::vector<float> zeros;                       // what every unfed input reads
    std::vector<float> spare;                       // where every unlistened output lands
    std::vector<std::vector<float*>> inPtrs, outPtrs;
    std::vector<AudioBusBuffers> inBuses, outBuses;
    ProcessContext context;
    ParamChanges inChanges, outChanges;
    NoEvents inEvents, outEvents;

    // parameters (the list belongs to the UI thread)
    struct Param {
        ParamID id = 0;
        int32_t flags = 0;
        std::string name, units;
    };
    std::vector<Param> params;                      // the ones meant to be shown, in the plug-in's order
    std::unordered_map<ParamID, int> paramIndex;
    std::atomic<bool> paramsStale{true};            // the plug-in said its parameters changed
    std::atomic<bool> valuesChanged{false};         // the plug-in changed many values at once (told to the app in idle())

    // parameter changes between the two threads
    struct Change {
        ParamID id;
        ParamValue value;
    };
    std::mutex changeMutex;
    std::vector<Change> toProcessor;                // UI -> audio; under changeMutex
    std::vector<Change> fromProcessor;              // audio -> UI (meters etc.); under changeMutex
    std::vector<Change> taken;                      // audio thread only
    std::vector<Change> shown;                      // UI thread only

    Vst3Plugin()
    {
        std::memset(&context, 0, sizeof(context));
        toProcessor.reserve(256);
        taken.reserve(256);
        fromProcessor.reserve(256);
        shown.reserve(256);
    }

    ~Vst3Plugin() override
    {
        closeEditor();
        deactivate();
        if (controller) controller->setComponentHandler(nullptr);
        if (handler) handler->forgetOwner();
        if (frame) frame->forgetOwner();
        if (linked) {
            componentLink->disconnect(componentSees);
            controllerLink->disconnect(controllerSees);
            linked = false;
        }
        if (componentSees) componentSees->cut();
        if (controllerSees) controllerSees->cut();
        drop(componentSees);
        drop(controllerSees);
        drop(componentLink);
        drop(controllerLink);
        if (component && componentStarted) component->terminate();
        if (controller && controllerStarted && !controllerIsComponent) controller->terminate();
        drop(processor);
        drop(controller);
        drop(component);
        drop(handler);
        drop(frame);
        // the file stays loaded
    }

    // ---------------------------------------------------------------- making it
    bool create(const AudioPluginDesc& d, std::string* error)
    {
        desc = d;
        desc.format = "VST3";
        module = loadModule(d.path, error);
        if (!module) return false;
        TUID cid;
        if (!uidFromText(d.uid, cid)) {
            if (error) *error = "has no usable class id (\"" + d.uid + "\")";
            return false;
        }

        void* obj = nullptr;
        if (module->factory->createInstance(cid, IComponent_iid, &obj) != kResultOk || !obj) {
            if (error) *error = "did not start (the effect could not be made)";
            return false;
        }
        component = static_cast<IComponent*>(obj);
        if (component->initialize(hostApp()) != kResultOk) {
            if (error) *error = "did not start (the effect would not initialize)";
            return false;
        }
        componentStarted = true;
        processor = queryFor<IAudioProcessor>(component, IAudioProcessor_iid);
        if (!processor) {
            if (error) *error = "cannot process audio";
            return false;
        }

        // The controller: the same object, or its own, made from the class the component names. A plug-in whose
        // controller cannot be had still processes; it just has no parameters and no window.
        controller = queryFor<IEditController>(component, IEditController_iid);
        if (controller) {
            controllerIsComponent = true;
        } else {
            TUID ccid;
            std::memset(ccid, 0, sizeof(ccid));
            const TUID none = {0};
            if (component->getControllerClassId(ccid) == kResultOk && !iidEqual(ccid, none)) {
                obj = nullptr;
                if (module->factory->createInstance(ccid, IEditController_iid, &obj) == kResultOk && obj) {
                    controller = static_cast<IEditController*>(obj);
                    if (controller->initialize(hostApp()) == kResultOk) controllerStarted = true;
                    else drop(controller);
                }
            }
        }
        if (controller) {
            if (!controllerIsComponent) {
                componentLink = queryFor<IConnectionPoint>(component, IConnectionPoint_iid);
                controllerLink = queryFor<IConnectionPoint>(controller, IConnectionPoint_iid);
                if (componentLink && controllerLink) {
                    componentSees = new Link(controllerLink);
                    controllerSees = new Link(componentLink);
                    componentLink->connect(componentSees);
                    controllerLink->connect(controllerSees);
                    linked = true;
                }
            }
            handler = new Handler(this);
            controller->setComponentHandler(handler);
            giveControllerComponentState();
        }

        setUpBuses();
        readBuses();
        desc.numInputs = inChannels.empty() ? 0 : inChannels[0];
        desc.numOutputs = outChannels.empty() ? 0 : outChannels[0];
        if (const Module::Class* c = module->find(cid)) {
            if (desc.name.empty()) desc.name = c->name;
            if (desc.vendor.empty()) desc.vendor = c->vendor.empty() ? module->vendor : c->vendor;
            desc.isInstrument = c->subCategories.find("Instrument") != std::string::npos;
        }
        if (desc.name.empty()) desc.name = pathStem(d.path);
        refreshParams();
        return true;
    }

    // The controller shows the component's values: it is handed the component's state to read them from.
    void giveControllerComponentState()
    {
        if (!component || !controller) return;
        MemStream* s = new MemStream();
        if (component->getState(s) == kResultOk) {
            s->pos = 0;
            controller->setComponentState(s);
        }
        s->release();
    }

    static int busCount(int32_t n) { return n < 0 ? 0 : (n > 64 ? 64 : (int)n); }

    // Asks for stereo in and out on the main buses (the plug-in may settle on something else, mono for one),
    // switches the main buses on and other audio buses off. 'main': what the main buses are asked for (5.1 for
    // setChannels(6)).
    void setUpBuses(SpeakerArrangement main = SpeakerArr::kStereo)
    {
        const int nIn = busCount(component->getBusCount(kAudio, kInput));
        const int nOut = busCount(component->getBusCount(kAudio, kOutput));
        std::vector<SpeakerArrangement> in((size_t)nIn, SpeakerArr::kEmpty), out((size_t)nOut, SpeakerArr::kEmpty);
        for (int dir = 0; dir < 2; ++dir) {             // other buses are asked to stay as they are
            std::vector<SpeakerArrangement>& list = dir == 0 ? in : out;
            const BusDirection d = dir == 0 ? kInput : kOutput;
            for (size_t k = 0; k < list.size(); ++k) {
                SpeakerArrangement a = SpeakerArr::kEmpty;
                if (processor->getBusArrangement(d, (int32_t)k, a) != kResultOk) {
                    BusInfo info;
                    std::memset(&info, 0, sizeof(info));
                    a = SpeakerArr::kEmpty;
                    if (component->getBusInfo(kAudio, d, (int32_t)k, info) == kResultOk)
                        a = info.channelCount == 1 ? SpeakerArr::kMono : (info.channelCount == 2 ? SpeakerArr::kStereo : SpeakerArr::kEmpty);
                }
                list[k] = a;
            }
        }
        if (nIn > 0) in[0] = main;
        if (nOut > 0) out[0] = main;
        if (nIn + nOut > 0) processor->setBusArrangements(nIn ? in.data() : nullptr, nIn, nOut ? out.data() : nullptr, nOut);
        for (int k = 0; k < nIn; ++k) component->activateBus(kAudio, kInput, k, (TBool)(k == 0 ? 1 : 0));
        for (int k = 0; k < nOut; ++k) component->activateBus(kAudio, kOutput, k, (TBool)(k == 0 ? 1 : 0));
    }

    // How many channels each audio bus has now.
    void readBuses()
    {
        inChannels.clear();
        outChannels.clear();
        for (int dir = 0; dir < 2; ++dir) {
            std::vector<int>& list = dir == 0 ? inChannels : outChannels;
            const int n = busCount(component->getBusCount(kAudio, dir == 0 ? kInput : kOutput));
            for (int k = 0; k < n; ++k) {
                BusInfo info;
                std::memset(&info, 0, sizeof(info));
                int ch = 0;
                if (component->getBusInfo(kAudio, dir == 0 ? kInput : kOutput, k, info) == kResultOk) ch = info.channelCount;
                list.push_back(ch < 0 ? 0 : (ch > 64 ? 64 : ch));
            }
        }
    }

    // ---------------------------------------------------------------- audio
    void stopLocked()
    {
        if (!active) return;
        processor->setProcessing(0);
        component->setActive(0);
        active = false;
    }

    bool activate(double sr, int maxBlock) override
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!component || !processor) return false;
        stopLocked();
        sampleRate = sr > 0.0 ? sr : 48000.0;
        blockSize = maxBlock < 16 ? 16 : maxBlock;
        return activateLocked();
    }

    // ---- six channels (setChannels): the main buses asked for 5.1 (kSpeakerArr51). The plug-in takes it if both
    // main buses then say 5.1 (VST3 lets a plug-in refuse or settle on something else; then it stays stereo).
    bool setChannels(int n) override
    {
        if (n != 2 && n != 6) return false;
        std::lock_guard<std::mutex> lk(mutex);
        if (!component || !processor) return n == 2;
        if (n == chans) return true;
        const bool wasActive = active;
        stopLocked();
        bool ok = true;
        if (n == 6) {
            setUpBuses(SpeakerArr::k51);
            SpeakerArrangement a = SpeakerArr::kEmpty, b = SpeakerArr::kEmpty;
            const int nIn = busCount(component->getBusCount(kAudio, kInput)), nOut = busCount(component->getBusCount(kAudio, kOutput));
            ok = nIn > 0 && nOut > 0 && processor->getBusArrangement(kInput, 0, a) == kResultOk &&
                 processor->getBusArrangement(kOutput, 0, b) == kResultOk && a == SpeakerArr::k51 && b == SpeakerArr::k51;
            if (ok) {
                readBuses();
                ok = !inChannels.empty() && !outChannels.empty() && inChannels[0] == 6 && outChannels[0] == 6;
            }
            if (!ok) setUpBuses();
        } else {
            setUpBuses();
        }
        chans = ok ? n : 2;
        if (wasActive) activateLocked();
        return ok;
    }
    int channels() const override { return chans; }

    void processChannels(float* const* io, int channels, int frames, int64_t samplePos, bool playing) override
    {
        if (chans != 6 || channels < 6) {
            if (channels >= 2) process(io[0], io[1], frames, samplePos, playing);
            return;
        }
        runBlocks(io, 6, frames, samplePos, playing);
    }

    bool activateLocked()
    {
        ProcessSetup setup;
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = blockSize;
        setup.sampleRate = sampleRate;
        tresult r = processor->setupProcessing(setup);
        if (r != kResultOk && r != kNotImplemented) return false;

        // buffers, and the bus descriptions process() hands over (pointers are set for every block)
        readBuses();
        for (int c = 0; c < 6; ++c) { inBuf[c].assign((size_t)blockSize, 0.f); outBuf[c].assign((size_t)blockSize, 0.f); }
        zeros.assign((size_t)blockSize, 0.f);
        spare.assign((size_t)blockSize, 0.f);
        inPtrs.assign(inChannels.size(), std::vector<float*>());
        outPtrs.assign(outChannels.size(), std::vector<float*>());
        for (size_t b = 0; b < inChannels.size(); ++b) inPtrs[b].assign((size_t)std::max(inChannels[b], 1), nullptr);
        for (size_t b = 0; b < outChannels.size(); ++b) outPtrs[b].assign((size_t)std::max(outChannels[b], 1), nullptr);
        inBuses.assign(inChannels.size(), AudioBusBuffers());
        outBuses.assign(outChannels.size(), AudioBusBuffers());
        inChanges.reserve(std::min<size_t>(params.size() + 16, 4096));
        outChanges.reserve(64);

        std::memset(&context, 0, sizeof(context));
        context.sampleRate = sampleRate;
        context.tempo = 120.0;
        context.timeSigNumerator = 4;
        context.timeSigDenominator = 4;
        blocksDone = 0;

        r = component->setActive(1);
        if (r != kResultOk && r != kNotImplemented) return false;
        readLatency();                              // only final once the plug-in is active
        processor->setProcessing(1);                // many plug-ins answer "not implemented"
        active = true;
        return true;
    }

    void deactivate() override
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!component || !processor) return;
        stopLocked();
    }

    // Points every bus at its buffers. Main input: the first two channels are ours, the rest silence. Other input
    // buses: silence (flagged). Main output: the first two channels are ours. Everything else goes to a buffer
    // nobody reads.
    void pointBuses()
    {
        for (size_t b = 0; b < inBuses.size(); ++b) {
            std::vector<float*>& ptrs = inPtrs[b];
            const int ch = inChannels[b];
            uint64_t silent = 0;
            for (size_t c = 0; c < ptrs.size(); ++c) {
                const bool ours = b == 0 && (int)c < chans && (int)c < ch;
                ptrs[c] = ours ? inBuf[c].data() : zeros.data();
                if (!ours && (int)c < ch) silent |= (uint64_t)1 << c;
            }
            inBuses[b].numChannels = ch;
            inBuses[b].silenceFlags = silent;
            inBuses[b].channelBuffers32 = ptrs.data();
        }
        for (size_t b = 0; b < outBuses.size(); ++b) {
            std::vector<float*>& ptrs = outPtrs[b];
            const int ch = outChannels[b];
            for (size_t c = 0; c < ptrs.size(); ++c) ptrs[c] = (b == 0 && (int)c < chans && (int)c < ch) ? outBuf[c].data() : spare.data();
            outBuses[b].numChannels = ch;
            outBuses[b].silenceFlags = 0;
            outBuses[b].channelBuffers32 = ptrs.data();
        }
    }

    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override
    {
        if (!left || !right) return;
        float* io[2] = {left, right};
        runBlocks(io, 2, frames, samplePos, playing);
    }

    // Runs 'nio' buffers (2, or 6 when the main buses are 5.1) through the plug-in in place, a block at a time.
    void runBlocks(float* const* io, int nio, int frames, int64_t samplePos, bool playing)
    {
        std::unique_lock<std::mutex> lk(mutex, std::try_to_lock);
        if (!lk.owns_lock() || !processor || !active || frames <= 0 || crashed) return;     // crashed: sound passes unchanged
        if (nio > chans) nio = chans;
        float* const left = io[0];
        float* const right = io[1];
        const int mainIn = inChannels.empty() ? 0 : inChannels[0];
        const int mainOut = outChannels.empty() ? 0 : outChannels[0];

        // what the UI thread changed since the last block
        inChanges.clear();
        {
            std::unique_lock<std::mutex> cl(changeMutex, std::try_to_lock);
            if (cl.owns_lock()) taken.swap(toProcessor);        // otherwise with the next block
        }
        for (const Change& c : taken) {
            int32_t index = 0;
            if (IParamValueQueue* q = inChanges.addParameterData(c.id, index)) q->addPoint(0, c.value, index);
        }
        taken.clear();

        int done = 0;
        while (done < frames) {
            const int n = std::min(blockSize, frames - done);
            const size_t bytes = (size_t)n * sizeof(float);
            if (mainIn == 1) {
                for (int k = 0; k < n; ++k) inBuf[0][(size_t)k] = 0.5f * (left[done + k] + right[done + k]);
            } else if (mainIn >= 2) {
                for (int c = 0; c < chans; ++c) {
                    if (c < nio) std::memcpy(inBuf[c].data(), io[c] + done, bytes);
                    else std::memset(inBuf[c].data(), 0, bytes);        // stereo handed to a 5.1 plug-in: the rest silent
                }
            }
            std::memset(zeros.data(), 0, bytes);            // in case a plug-in wrote into its input
            for (int c = 0; c < chans; ++c) std::memset(outBuf[c].data(), 0, bytes);
            pointBuses();

            const int64_t pos = samplePos + done;
            context.state = (uint32_t)ProcessContext::kTempoValid | (uint32_t)ProcessContext::kTimeSigValid |
                            (uint32_t)ProcessContext::kProjectTimeMusicValid | (uint32_t)ProcessContext::kBarPositionValid |
                            (uint32_t)ProcessContext::kContTimeValid | (playing ? (uint32_t)ProcessContext::kPlaying : 0u);
            context.sampleRate = sampleRate;
            context.projectTimeSamples = pos;
            context.continousTimeSamples = blocksDone;
            context.projectTimeMusic = (double)pos / sampleRate * (context.tempo / 60.0);
            context.barPositionMusic = std::floor(context.projectTimeMusic / 4.0) * 4.0;

            ProcessData data;
            data.processMode = kRealtime;
            data.symbolicSampleSize = kSample32;
            data.numSamples = n;
            data.numInputs = (int32_t)inBuses.size();
            data.numOutputs = (int32_t)outBuses.size();
            data.inputs = inBuses.empty() ? nullptr : inBuses.data();
            data.outputs = outBuses.empty() ? nullptr : outBuses.data();
            data.inputParameterChanges = &inChanges;
            data.outputParameterChanges = &outChanges;
            data.inputEvents = &inEvents;
            data.outputEvents = &outEvents;
            data.processContext = &context;
            outChanges.clear();

            tInProcess = true;
#ifdef _WIN32
            unsigned long code = 0;
            const tresult r = guardedProcess(processor, data, &code);
            if (code) {
                tInProcess = false;
                crashed = true;
                logf("VST3 %s crashed while processing (exception 0x%08lX): it is not used again", desc.name.c_str(), code);
                return;
            }
#else
            const tresult r = processor->process(data);
#endif
            tInProcess = false;
            blocksDone += n;
            inChanges.clear();                              // the changes went in with the first piece
            if (r == kResultOk && mainOut >= 1) {
                // a channel the plug-in says is silent is silent whatever its buffer holds
                const uint64_t silent = outBuses[0].silenceFlags;
                for (int c = 0; c < chans && c < mainOut; ++c)
                    if (silent & ((uint64_t)1 << c)) std::memset(outBuf[c].data(), 0, bytes);
                std::memcpy(left + done, outBuf[0].data(), bytes);
                std::memcpy(right + done, (mainOut >= 2 ? outBuf[1] : outBuf[0]).data(), bytes);
                for (int c = 2; c < nio; ++c) std::memcpy(io[c] + done, (c < mainOut ? outBuf[c] : zeros).data(), bytes);
            }
            if (outChanges.getParameterCount() > 0) keepOutputChanges();
            done += n;
        }
    }

    // What the processor changed itself (meters etc.), kept for idle() to tell the controller.
    void keepOutputChanges()
    {
        std::unique_lock<std::mutex> cl(changeMutex, std::try_to_lock);
        if (!cl.owns_lock()) return;
        const int32_t count = outChanges.getParameterCount();
        for (int32_t k = 0; k < count; ++k) {
            ParamQueue* q = outChanges.queue(k);
            if (!q || q->points.empty()) continue;
            const ParamValue value = q->points.back().value;
            bool known = false;
            for (Change& c : fromProcessor)
                if (c.id == q->id) { c.value = value; known = true; break; }
            if (!known && fromProcessor.size() < fromProcessor.capacity()) {     // never grows here: no allocation on the audio thread
                Change c;
                c.id = q->id;
                c.value = value;
                fromProcessor.push_back(c);
            }
        }
    }

    void readLatency()
    {
        const uint32_t n = processor->getLatencySamples();
        latency = n > 0x7FFFFFFFu ? 0 : (int)n;
        latencyKnown = true;
    }

    int latencySamples() override
    {
        if (!processor) return 0;
        if (!latencyKnown) readLatency();
        return latency;
    }

    // ---------------------------------------------------------------- parameters
    void refreshParams()
    {
        if (!paramsStale.exchange(false)) return;
        params.clear();
        paramIndex.clear();
        if (!controller) return;
        int32_t count = controller->getParameterCount();
        if (count < 0) count = 0;
        if (count > 100000) count = 100000;
        for (int32_t k = 0; k < count; ++k) {
            ParameterInfo info;
            std::memset(&info, 0, sizeof(info));
            if (controller->getParameterInfo(k, info) != kResultOk) continue;
            if (info.flags & ParameterInfo::kIsHidden) continue;            // not meant to be shown
            Param p;
            p.id = info.id;
            p.flags = info.flags;
            p.name = fromUtf16(info.title, 128);
            if (p.name.empty()) p.name = fromUtf16(info.shortTitle, 128);
            p.units = fromUtf16(info.units, 128);
            if (paramIndex.count(p.id)) continue;
            paramIndex[p.id] = (int)params.size();
            params.push_back(p);
        }
    }

    const Param* param(int index)
    {
        refreshParams();
        return (controller && index >= 0 && index < (int)params.size()) ? &params[(size_t)index] : nullptr;
    }

    int paramCount() override
    {
        refreshParams();
        return (int)params.size();
    }

    std::string paramName(int index) override
    {
        const Param* p = param(index);
        return p ? p->name : std::string();
    }

    std::string paramText(int index) override
    {
        const Param* p = param(index);
        if (!p) return std::string();
        const ParamValue v = controller->getParamNormalized(p->id);
        String128 text;
        std::memset(text, 0, sizeof(text));
        std::string s;
        if (controller->getParamStringByValue(p->id, v, text) == kResultOk) s = fromUtf16(text, 128);
        else s = strfmt("%.3f", v);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return p->units.empty() ? s : s + " " + p->units;
    }

    float paramGet(int index) override
    {
        const Param* p = param(index);
        if (!p) return 0.f;
        const ParamValue v = controller->getParamNormalized(p->id);
        return v < 0.0 ? 0.f : (v > 1.0 ? 1.f : (float)v);
    }

    void paramSet(int index, float v) override
    {
        const Param* p = param(index);
        if (!p || (p->flags & ParameterInfo::kIsReadOnly) || !(v == v)) return;
        const ParamValue value = v < 0.f ? 0.0 : (v > 1.f ? 1.0 : (ParamValue)v);
        controller->setParamNormalized(p->id, value);       // the window's half
        queueChange(p->id, value);                          // the audio half, with its next block
    }

    void queueChange(ParamID id, ParamValue value)
    {
        std::lock_guard<std::mutex> cl(changeMutex);
        for (Change& c : toProcessor)
            if (c.id == id) { c.value = value; return; }    // only the newest value matters
        Change c;
        c.id = id;
        c.value = value;
        toProcessor.push_back(c);
    }

    // From the plug-in's own window (Handler).
    tresult edited(ParamID id, ParamValue value)
    {
        queueChange(id, value);
        auto it = paramIndex.find(id);
        if (it != paramIndex.end() && onParamChangedByPlugin) onParamChangedByPlugin(it->second);
        return kResultOk;
    }

    tresult restart(int32_t flags)
    {
        // Not acted on inside the plug-in's call: the list is re-read when next asked for, and the application is
        // told about changed values in idle().
        if (flags & (kParamTitlesChanged | kReloadComponent | kParamIDMappingChanged)) paramsStale = true;
        if (flags & (kParamValuesChanged | kReloadComponent)) valuesChanged = true;
        if (flags & (kLatencyChanged | kReloadComponent)) latencyKnown = false;
        return kResultOk;
    }

    void idle() override
    {
        if (!controller) return;
        // values the processor changed itself go to the controller, whose window shows them
        {
            std::lock_guard<std::mutex> cl(changeMutex);
            shown.swap(fromProcessor);
        }
        for (const Change& c : shown) controller->setParamNormalized(c.id, c.value);
        shown.clear();
        if (valuesChanged.exchange(false)) {
            refreshParams();
            if (onParamChangedByPlugin)
                for (int k = 0; k < (int)params.size(); ++k) onParamChangedByPlugin(k);
        }
    }

    // ---------------------------------------------------------------- state
    // "VST3:<component state>:<controller state>", each base64, or "-" for a half that gave none.
    std::string saveState() override
    {
        if (!component) return std::string();
        std::string parts[2] = {"-", "-"};
        MemStream* s = new MemStream();
        if (component->getState(s) == kResultOk) parts[0] = base64Encode(s->data.data(), s->data.size());
        s->release();
        if (controller) {
            s = new MemStream();
            if (controller->getState(s) == kResultOk) parts[1] = base64Encode(s->data.data(), s->data.size());
            s->release();
        }
        return "VST3:" + parts[0] + ":" + parts[1];
    }

    bool loadState(const std::string& text) override
    {
        if (!component || text.compare(0, 5, "VST3:") != 0) return false;
        const size_t sep = text.find(':', 5);
        if (sep == std::string::npos) return false;
        const std::string a = text.substr(5, sep - 5), b = text.substr(sep + 1);
        bool ok = true;
        if (a != "-") {
            MemStream* s = new MemStream(base64Decode(a));
            {
                std::lock_guard<std::mutex> lk(mutex);          // not in the middle of a block
                ok = component->setState(s) == kResultOk;
            }
            if (controller) {                                   // so the controller shows the component's new state
                s->pos = 0;
                controller->setComponentState(s);
            }
            s->release();
        }
        if (b != "-" && controller) {
            MemStream* s = new MemStream(base64Decode(b));
            const bool ctl = controller->setState(s) == kResultOk;
            if (a == "-") ok = ctl;
            s->release();
        }
        {
            std::lock_guard<std::mutex> cl(changeMutex);        // changes made before this must not land on top of it
            toProcessor.clear();
        }
        return ok;
    }

    // ---------------------------------------------------------------- the plug-in's own window
    bool hasEditor() override
    {
        if (!controller) return false;
        if (view) return true;
        if (editorKnown < 0) {                      // the only way to find out is to ask for one
            IPlugView* v = controller->createView(ViewType::kEditor);
            editorKnown = (v && v->isPlatformTypeSupported(kWindowType) == kResultOk) ? 1 : 0;
            drop(v);
        }
        return editorKnown > 0;
    }

    bool openEditor(void* parentWindow, int* w, int* h) override
    {
        if (!controller || !parentWindow) return false;
        if (view) closeEditor();
        if (!hasEditor()) return false;
        view = controller->createView(ViewType::kEditor);
        if (!view) return false;
        if (!frame) frame = new Frame(this);
        view->setFrame(frame);
        if (view->isPlatformTypeSupported(kWindowType) == kResultFalse) {     // a window for another system only
            drop(view);
            return false;
        }
#ifdef _WIN32
        unsigned long code = 0;
        const tresult attachedOk = guardedAttach(view, parentWindow, kWindowType, &code);
        if (code) {
            logf("VST3 %s crashed opening its window (exception 0x%08lX)", desc.name.c_str(), code);
            view = nullptr;                         // not released: it cannot safely be called again
            return false;
        }
        if (attachedOk != kResultOk) {
#else
        if (view->attached(parentWindow, kWindowType) != kResultOk) {
#endif
            view->setFrame(nullptr);
            drop(view);
            return false;
        }
        viewAttached = true;
        ViewRect r;
        int ew = 0, eh = 0;
        if (view->getSize(&r) == kResultOk) { ew = r.getWidth(); eh = r.getHeight(); }
        if (ew <= 0 || eh <= 0) { ew = 400; eh = 300; }
        if (w) *w = ew;
        if (h) *h = eh;
        return true;
    }

    void closeEditor() override
    {
        if (!view) return;
        if (viewAttached) view->removed();
        viewAttached = false;
        view->setFrame(nullptr);
        drop(view);
    }

    // From the plug-in's window (Frame): it wants to be this size.
    tresult resized(IPlugView* v, ViewRect* newSize)
    {
        if (!v || !newSize) return kInvalidArgument;
        if (resizing) return kResultFalse;          // asked again from inside onSize
        resizing = true;
        ViewRect r = *newSize;
        if (onEditorResize && r.getWidth() > 0 && r.getHeight() > 0) onEditorResize(r.getWidth(), r.getHeight());
        v->onSize(&r);
        resizing = false;
        return kResultOk;
    }
};

tresult PLUGIN_API Handler::performEdit(ParamID id, ParamValue valueNormalized)
{
    Vst3Plugin* owner = owner_;
    return owner ? owner->edited(id, valueNormalized) : kResultFalse;
}

tresult PLUGIN_API Handler::restartComponent(int32_t flags)
{
    Vst3Plugin* owner = owner_;
    return owner ? owner->restart(flags) : kResultOk;
}

tresult PLUGIN_API Frame::resizeView(IPlugView* view, ViewRect* newSize)
{
    Vst3Plugin* owner = owner_;
    return owner ? owner->resized(view, newSize) : kResultFalse;
}

}  // namespace

std::vector<AudioPluginDesc> vst3Describe(const std::string& path, std::string* error)
{
    std::vector<AudioPluginDesc> out;
    Module* mod = loadModule(path, error);
    if (!mod) return out;
    for (const auto& c : mod->classes) {
        if (c.category != kVstAudioEffectClass) continue;       // the other kind in a file is the controller half
        AudioPluginDesc d;
        d.format = "VST3";
        d.path = path;
        d.uid = uidToText(c.cid);
        d.name = c.name.empty() ? pathStem(path) : c.name;
        d.vendor = c.vendor.empty() ? mod->vendor : c.vendor;
        d.hasEditor = true;                                     // only known once one is made: hasEditor()
        d.isInstrument = c.subCategories.find("Instrument") != std::string::npos;
        out.push_back(d);
    }
    if (out.empty() && error) *error = "has no audio effects in it";
    return out;
}

std::unique_ptr<AudioPlugin> vst3Create(const AudioPluginDesc& desc, std::string* error)
{
    std::unique_ptr<Vst3Plugin> p(new Vst3Plugin());
    if (!p->create(desc, error)) return nullptr;
    return std::unique_ptr<AudioPlugin>(p.release());
}

}  // namespace reno
