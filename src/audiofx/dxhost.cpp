// DirectX audio effect host (Windows only; the two entry points are stubs elsewhere).
//
// "DirectX plug-ins" are two different things, both hosted here behind AudioPlugin. The uid says which:
//
//   "dmo:{clsid}"    A DirectX Media Object registered as an audio effect (DMOCATEGORY_AUDIO_EFFECT). Windows ships
//                    some in dsdmo.dll (Chorus, Compressor, Distortion, Echo, Flanger, Gargle, I3DL2 Reverb, ParamEq,
//                    WavesReverb). Driven directly: IMediaObjectInPlace::Process if the object has it, else
//                    ProcessInput / ProcessOutput. Parameters come from IMediaParamInfo / IMediaParams.
//
//   "dshow:{clsid}"  A DirectShow audio transform filter: what Sound Forge / Vegas / Cakewalk call a DirectX plug-in
//                    (one audio input pin, one audio output pin, normally with property pages). The filter is put in
//                    a private filter graph between two small filters of our own:
//
//                        [DxFilter with an output pin] -> [the effect] -> [DxFilter with an input pin]
//
//                    The graph renders nothing; it gives the effect a graph to belong to, makes the two connections
//                    (ConnectDirect) and starts and stops the three filters in order. There is no reference clock.
//                    process() hands one media sample per block to the effect's input pin (IMemInputPin::Receive);
//                    what the effect delivers to our input pin goes into a small FIFO that process() then empties.
//
// Sample format: 32-bit float stereo at the project's sample rate is asked for first; if the effect refuses,
// 16-bit PCM stereo with conversion on the way in and out.
//
// Threads:
//   * dxCreate(), openEditor(), closeEditor(), idle() and the destructor run on one thread, the UI thread, which is
//     (or is made here) a single-threaded COM apartment. The effect is created on it, in process, so a property page
//     gets the direct pointer to its effect that it expects.
//   * process() runs on an audio thread, which joins COM's multithreaded apartment the first time it gets here
//     (CoInitializeEx(COINIT_MULTITHREADED)) because effects may use COM while processing. The effect is called
//     through the same direct pointers as on the UI thread, with no marshalling, as DirectShow's own streaming
//     threads call filters; an effect that needs marshalling cannot be hosted by anybody.
//   * activate(), deactivate(), saveState(), loadState() and the parameter functions may run on any thread that is
//     not inside process(); a thread with no COM yet joins the multithreaded apartment.
//   * COM initialisation done here is never undone: undoing it at thread exit would run under the loader lock.
//   * Each plug-in's mutex keeps process() apart from activate / deactivate / loadState. process() only tries the
//     lock: while one of those runs it leaves the audio untouched instead of waiting.
//   * The graph manager also calls our two filters from its own worker thread (an in-place transform asks for its
//     input to be reconnected once its output is connected). Each DxFilter has a lock for its connection and state;
//     samples arriving at the input pin take only that pin's data lock.
//
// Link with: strmiids.lib msdmo.lib dmoguids.lib ole32.lib oleaut32.lib uuid.lib (plus user32 and kernel32).
#include "audioplugin.h"

#ifndef _WIN32

namespace reno {

std::vector<AudioPluginDesc> listDirectXAudioPlugins() { return {}; }

std::unique_ptr<AudioPlugin> dxCreate(const AudioPluginDesc&, std::string* error)
{
    if (error) *error = "DirectX plug-ins exist only on Windows";
    return nullptr;
}

}  // namespace reno

#else  // _WIN32

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <ocidl.h>
#include <olectl.h>
#include <mmreg.h>
#include <strmif.h>
#include <control.h>
#include <uuids.h>
#include <vfwmsgs.h>
#include <mediaobj.h>
#include <medparam.h>
#include <mediaerr.h>
#include <dmoreg.h>
#include <dmodshow.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <set>

namespace reno {

namespace {

// ================================================================================================ COM basics

// A reference to a COM object (or to one of our own reference-counted objects).
template <class T> class ComPtr {
public:
    ComPtr() {}
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ~ComPtr() { reset(); }
    ComPtr& operator=(const ComPtr& o) { set(o.p_); return *this; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() { if (p_) { T* t = p_; p_ = nullptr; t->Release(); } }
    void set(T* t) { if (t) t->AddRef(); reset(); p_ = t; }          // shares 't'
    void attach(T* t) { reset(); p_ = t; }                           // takes over the caller's reference
    T** put() { reset(); return &p_; }                               // for functions that return a reference
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p_); }
private:
    T* p_ = nullptr;
};

template <class T> bool queryInterface(IUnknown* from, ComPtr<T>& to)
{
    to.reset();
    return from && SUCCEEDED(from->QueryInterface(__uuidof(T), to.putVoid())) && to;
}

// Makes sure the calling thread can use COM (see "Threads" at the top). 'sta': what the thread becomes if it has none.
bool comEnsure(bool sta)
{
    thread_local int state = 0;         // 0 not tried yet, 1 usable, -1 not usable
    if (state == 0) {
        const HRESULT hr = CoInitializeEx(nullptr, sta ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE: the thread already has COM the other way round, which is just as good
        state = (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) ? 1 : -1;
    }
    return state > 0;
}

// COM for the length of one function (listing), released afterwards.
class ScopedCom {
public:
    ScopedCom() { hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ScopedCom() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT hr_;
};

std::string guidText(const GUID& g)
{
    wchar_t buf[64] = {0};
    if (StringFromGUID2(g, buf, 64) <= 0) return std::string();
    return narrow(buf);
}

bool guidParse(const std::wstring& s, GUID& g) { return SUCCEEDED(IIDFromString(s.c_str(), &g)); }

// Why creating or connecting something failed, in words for the common codes.
std::string hrText(HRESULT hr)
{
    switch ((unsigned)hr) {
    case 0x80040154u: return "it is not registered on this machine";
    case 0x80040111u: return "its DLL has no such class";
    case 0x8007007Eu: return "its DLL, or a DLL that one needs, is missing";
    case 0x800700C1u: return "it is a 32-bit plug-in and this is a 64-bit program";
    case 0x80004002u: return "it does not have the interface an audio effect needs";
    case 0x8007000Eu: return "out of memory";
    case 0x80040207u: return "no sample format both sides accept";
    case 0x8004022Au: return "it does not accept the sample format";
    case 0x80040266u: return "it does not take samples the ordinary way";
    case 0x80040217u: return "the connection needs other filters in between";
    case 0x80040209u: return "it did not stay connected";
    default: return strfmt("error 0x%08X", (unsigned)hr);
    }
}

void copyWide(wchar_t* dst, size_t cap, const wchar_t* src)
{
    size_t n = 0;
    if (src) while (n + 1 < cap && src[n]) { dst[n] = src[n]; ++n; }
    dst[n] = 0;
}

// ================================================================================================ samples

// What the bytes going to or coming from an effect are.
struct PcmFormat {
    bool isFloat = true;
    int bits = 32;              // per sample, as stored
    int channels = 2;
    int rate = 48000;
    int blockAlign() const { return channels * (bits / 8); }
    bool operator==(const PcmFormat& o) const { return isFloat == o.isFloat && bits == o.bits && channels == o.channels && rate == o.rate; }
};

PcmFormat stereoFormat(bool isFloat, int rate)
{
    PcmFormat f;
    f.isFloat = isFloat;
    f.bits = isFloat ? 32 : 16;
    f.channels = 2;
    f.rate = rate;
    return f;
}

void fillWave(WAVEFORMATEX& wf, const PcmFormat& f)
{
    std::memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = (WORD)(f.isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
    wf.nChannels = (WORD)f.channels;
    wf.nSamplesPerSec = (DWORD)f.rate;
    wf.wBitsPerSample = (WORD)f.bits;
    wf.nBlockAlign = (WORD)f.blockAlign();
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = 0;
}

// Reads a WAVEFORMATEX (or the older PCMWAVEFORMAT, two bytes shorter) of 'cb' bytes. False unless plain PCM or float.
bool formatFromWave(const BYTE* data, size_t cb, PcmFormat& out)
{
    if (!data || cb < sizeof(PCMWAVEFORMAT)) return false;
    WAVEFORMATEX wf;
    std::memset(&wf, 0, sizeof(wf));
    std::memcpy(&wf, data, std::min(cb, sizeof(WAVEFORMATEX)));
    WORD tag = wf.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (cb < sizeof(WAVEFORMATEXTENSIBLE) || wf.cbSize < 22) return false;
        WAVEFORMATEXTENSIBLE wfx;
        std::memcpy(&wfx, data, sizeof(wfx));
        if (wfx.SubFormat == MEDIASUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
        else if (wfx.SubFormat == MEDIASUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else return false;
    }
    PcmFormat f;
    f.bits = wf.wBitsPerSample;
    f.channels = wf.nChannels;
    f.rate = (int)wf.nSamplesPerSec;
    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        f.isFloat = true;
        if (f.bits != 32 && f.bits != 64) return false;
    } else if (tag == WAVE_FORMAT_PCM) {
        f.isFloat = false;
        if (f.bits != 8 && f.bits != 16 && f.bits != 24 && f.bits != 32) return false;
    } else {
        return false;
    }
    if (f.channels < 1 || f.channels > 16 || f.rate < 1000 || f.rate > 768000) return false;
    if ((int)wf.nBlockAlign != f.blockAlign()) return false;
    out = f;
    return true;
}

inline int16_t toInt16(float v)
{
    const float s = v * 32768.f;
    if (!(s == s)) return 0;            // NaN
    if (s >= 32767.f) return 32767;
    if (s <= -32768.f) return -32768;
    return (int16_t)std::lrintf(s);
}

// Two channels into interleaved bytes. 'f' is one of the two formats we send: float or 16-bit, stereo.
void packStereo(const float* left, const float* right, int frames, const PcmFormat& f, BYTE* dst)
{
    if (f.isFloat) {
        for (int k = 0; k < frames; ++k) {
            const float pair[2] = { left[k], right[k] };
            std::memcpy(dst + (size_t)k * 8, pair, 8);
        }
    } else {
        for (int k = 0; k < frames; ++k) {
            const int16_t pair[2] = { toInt16(left[k]), toInt16(right[k]) };
            std::memcpy(dst + (size_t)k * 4, pair, 4);
        }
    }
}

inline float readSample(const BYTE* p, const PcmFormat& f)
{
    if (f.isFloat) {
        if (f.bits == 32) { float v; std::memcpy(&v, p, 4); return v; }
        double d;
        std::memcpy(&d, p, 8);
        return (float)d;
    }
    switch (f.bits) {
    case 8: return (float)((int)p[0] - 128) / 128.f;
    case 16: { int16_t v; std::memcpy(&v, p, 2); return (float)v / 32768.f; }
    case 24: { const int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)(int8_t)p[2] << 16); return (float)v / 8388608.f; }
    default: { int32_t v; std::memcpy(&v, p, 4); return (float)(v / 2147483648.0); }
    }
}

// One frame of any format formatFromWave lets through: the first two channels, or the only one twice.
inline void readFrame(const BYTE* p, const PcmFormat& f, float& left, float& right)
{
    left = readSample(p, f);
    right = f.channels > 1 ? readSample(p + f.bits / 8, f) : left;
}

// What an effect has produced and process() has not yet handed on. Fixed size (no allocation on the audio thread);
// when full, the oldest audio goes.
struct StereoFifo {
    std::vector<float> l, r;
    size_t head = 0, count = 0;

    void setCapacity(size_t frames) { l.assign(frames, 0.f); r.assign(frames, 0.f); head = count = 0; }
    void clear() { head = count = 0; }
    void push(float a, float b)
    {
        const size_t cap = l.size();
        if (cap == 0) return;
        size_t pos = head + count;
        if (pos >= cap) pos -= cap;
        l[pos] = a;
        r[pos] = b;
        if (count < cap) ++count;
        else if (++head == cap) head = 0;
    }
    void pushBytes(const BYTE* data, size_t bytes, const PcmFormat& f)
    {
        const size_t step = (size_t)f.blockAlign();
        if (step == 0) return;
        for (size_t n = bytes / step; n > 0; --n, data += step) {
            float a = 0.f, b = 0.f;
            readFrame(data, f, a, b);
            push(a, b);
        }
    }
    bool pop(float* outL, float* outR, size_t frames)        // all of 'frames' or nothing
    {
        if (count < frames) return false;
        const size_t cap = l.size();
        for (size_t k = 0; k < frames; ++k) {
            outL[k] = l[head];
            outR[k] = r[head];
            if (++head == cap) head = 0;
        }
        count -= frames;
        return true;
    }
};

size_t fifoFramesFor(int rate, int maxBlock) { return (size_t)std::max(rate / 2, maxBlock * 4); }

inline REFERENCE_TIME framesToTime(int64_t frames, int rate) { return rate > 0 ? frames * 10000000 / rate : 0; }

// ================================================================================================ state

// An object's own saved form (IPersistStream), as bytes.
bool persistSave(IPersistStream* ps, std::string& bytes)
{
    bytes.clear();
    if (!ps) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) || !stream) return false;
    if (FAILED(ps->Save(stream.get(), TRUE))) return false;
    STATSTG st;
    std::memset(&st, 0, sizeof(st));
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    if (st.cbSize.QuadPart > (ULONGLONG)256 * 1024 * 1024) return false;
    const size_t size = (size_t)st.cbSize.QuadPart;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    bytes.resize(size);
    size_t done = 0;
    while (done < size) {
        ULONG got = 0;
        const ULONG want = (ULONG)std::min(size - done, (size_t)1 << 20);
        if (FAILED(stream->Read(&bytes[done], want, &got)) || got == 0) break;
        done += got;
    }
    if (done != size) { bytes.clear(); return false; }
    return true;
}

bool persistLoad(IPersistStream* ps, const std::string& bytes)
{
    if (!ps) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) || !stream) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        ULONG wrote = 0;
        const ULONG want = (ULONG)std::min(bytes.size() - done, (size_t)1 << 20);
        if (FAILED(stream->Write(bytes.data() + done, want, &wrote)) || wrote == 0) return false;
        done += wrote;
    }
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    return SUCCEEDED(ps->Load(stream.get()));
}

// ================================================================================================ property pages

// What a property page talks to instead of a property sheet.
class PageSite final : public IPropertyPageSite {
public:
    std::atomic<bool> dirty{false};     // the page says the user changed something that is not applied yet

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IPropertyPageSite)) { *out = static_cast<IPropertyPageSite*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = --refs_;
        if (n == 0) delete this;
        return (ULONG)n;
    }
    STDMETHODIMP OnStatusChange(DWORD flags) override
    {
        // only noted here; applying happens from idle(), not inside the page's own message handling
        if (flags & PROPPAGESTATUS_DIRTY) dirty = true;
        return S_OK;
    }
    STDMETHODIMP GetLocaleID(LCID* id) override
    {
        if (!id) return E_POINTER;
        *id = GetUserDefaultLCID();
        return S_OK;
    }
    STDMETHODIMP GetPageContainer(IUnknown** container) override
    {
        if (container) *container = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }      // not handled

private:
    ~PageSite() {}
    std::atomic<long> refs_{1};
};

// The first property page of an effect, shown in a window of ours. All of it runs on the UI thread. (Keys the page
// would want from a property sheet, like Tab between controls, do not reach it: that needs
// IPropertyPage::TranslateAccelerator called from the program's message loop.)
// The property page 'id' made by the DLL that 'object' (an effect) lives in, through its DllGetClassObject.
bool pageFromEffectDll(const CLSID& id, IUnknown* object, ComPtr<IPropertyPage>& page)
{
    if (!object) return false;
    HMODULE dll = nullptr;
    void* vtable = *(void**)object;                 // an object's table of functions is in its DLL
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)vtable, &dll) || !dll)
        return false;
    typedef HRESULT(__stdcall * GetClassObject)(REFCLSID, REFIID, void**);
    GetClassObject get = (GetClassObject)(void*)GetProcAddress(dll, "DllGetClassObject");
    if (!get) return false;
    ComPtr<IClassFactory> factory;
    if (FAILED(get(id, __uuidof(IClassFactory), factory.putVoid())) || !factory) return false;
    return SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(IPropertyPage), page.putVoid())) && page;
}

class PageHost {
public:
    ~PageHost() { close(); }

    static bool hasPages(IUnknown* object)
    {
        ComPtr<ISpecifyPropertyPages> spp;
        if (!queryInterface(object, spp)) return false;
        CAUUID ids;
        ids.cElems = 0;
        ids.pElems = nullptr;
        const HRESULT hr = spp->GetPages(&ids);
        const bool any = SUCCEEDED(hr) && ids.cElems > 0 && ids.pElems;
        if (ids.pElems) CoTaskMemFree(ids.pElems);
        return any;
    }

    bool isOpen() const { return (bool)page_; }

    bool open(IUnknown* object, HWND parent, int* width, int* height)
    {
        close();
        if (!object || !parent || !IsWindow(parent)) return false;
        ComPtr<ISpecifyPropertyPages> spp;
        if (!queryInterface(object, spp)) return false;
        CAUUID ids;
        ids.cElems = 0;
        ids.pElems = nullptr;
        if (FAILED(spp->GetPages(&ids)) || !ids.pElems) {
            if (ids.pElems) CoTaskMemFree(ids.pElems);
            return false;
        }
        bool ok = false;
        for (ULONG k = 0; k < ids.cElems && !ok; ++k) ok = openPage(ids.pElems[k], object, parent, width, height);
        CoTaskMemFree(ids.pElems);
        return ok;
    }

    void close()
    {
        if (!page_) return;
        page_->Show(SW_HIDE);
        page_->Deactivate();
        page_->SetObjects(0, nullptr);
        page_->SetPageSite(nullptr);
        page_.reset();
        if (site_) site_->Release();
        site_ = nullptr;
        pollDirty_ = true;
    }

    // Has the page apply what the user changed to the effect. True if something was applied.
    bool applyIfDirty()
    {
        if (!page_) return false;
        bool dirty = site_ && site_->dirty.exchange(false);
        if (!dirty && pollDirty_) dirty = page_->IsPageDirty() == S_OK;
        if (!dirty) return false;
        const HRESULT hr = page_->Apply();
        // a page that stays dirty whatever we do is only listened to (OnStatusChange) from now on
        if (FAILED(hr) || page_->IsPageDirty() == S_OK) pollDirty_ = false;
        return hr == S_OK;
    }

private:
    bool openPage(const CLSID& id, IUnknown* object, HWND parent, int* width, int* height)
    {
        ComPtr<IPropertyPage> page;
        if (FAILED(CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IPropertyPage), page.putVoid())) || !page) {
            // A page that is not registered on its own (the effect's DLL registers only the effect) is asked of the
            // DLL the effect came from, as its own host would
            page.reset();
            if (!pageFromEffectDll(id, object, page)) return false;
        }
        PageSite* site = new PageSite();            // the one reference is ours
        HRESULT hr = page->SetPageSite(site);
        if (SUCCEEDED(hr)) {
            IUnknown* objects[1] = { object };
            hr = page->SetObjects(1, objects);
            if (SUCCEEDED(hr)) {
                PROPPAGEINFO info;
                std::memset(&info, 0, sizeof(info));
                info.cb = sizeof(info);
                int w = 0, h = 0;
                if (SUCCEEDED(page->GetPageInfo(&info))) { w = info.size.cx; h = info.size.cy; }
                CoTaskMemFree(info.pszTitle);
                CoTaskMemFree(info.pszDocString);
                CoTaskMemFree(info.pszHelpFile);
                if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { w = 400; h = 300; }
                RECT rc = { 0, 0, w, h };
                hr = page->Activate(parent, &rc, FALSE);
                if (SUCCEEDED(hr)) {
                    page->Show(SW_SHOW);
                    page_ = page;
                    site_ = site;
                    pollDirty_ = true;
                    if (width) *width = w;
                    if (height) *height = h;
                    return true;
                }
                page->SetObjects(0, nullptr);
            }
            page->SetPageSite(nullptr);
        }
        site->Release();
        return false;
    }

    ComPtr<IPropertyPage> page_;
    PageSite* site_ = nullptr;
    bool pollDirty_ = true;
};

// ================================================================================================ DMO effects

// The buffer object a DMO reads its input from or writes its output to.
class DxMediaBuffer final : public IMediaBuffer {
public:
    explicit DxMediaBuffer(size_t capacity) : data_(capacity) {}

    BYTE* bytes() { return data_.data(); }
    size_t capacity() const { return data_.size(); }
    DWORD length() const { return length_; }
    bool onlyOurs() const { return refs_.load() == 1; }       // the DMO has let go of it

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMediaBuffer)) { *out = static_cast<IMediaBuffer*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = --refs_;
        if (n == 0) delete this;
        return (ULONG)n;
    }
    STDMETHODIMP SetLength(DWORD n) override
    {
        if (n > data_.size()) return E_INVALIDARG;
        length_ = n;
        return S_OK;
    }
    STDMETHODIMP GetMaxLength(DWORD* n) override
    {
        if (!n) return E_POINTER;
        *n = (DWORD)data_.size();
        return S_OK;
    }
    STDMETHODIMP GetBufferAndLength(BYTE** buffer, DWORD* n) override
    {
        if (!buffer && !n) return E_POINTER;
        if (buffer) *buffer = data_.data();
        if (n) *n = length_;
        return S_OK;
    }

private:
    ~DxMediaBuffer() {}
    std::atomic<long> refs_{1};
    std::vector<BYTE> data_;
    DWORD length_ = 0;
};

// As in the VST hosts: a DirectX plug-in that crashes while processing (an access violation in its own code) is
// stopped there and not called again (the sound passes unchanged) instead of taking the program down.
// Separate functions: __try cannot be used where C++ objects are unwound.
HRESULT guardedInPlace(IMediaObjectInPlace* p, DWORD bytes, BYTE* data, REFERENCE_TIME t, unsigned long* code)
{
    __try {
        return p->Process(bytes, data, t, DMO_INPLACE_NORMAL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return E_FAIL;
    }
}
HRESULT guardedInput(IMediaObject* d, IMediaBuffer* b, DWORD flags, REFERENCE_TIME t, unsigned long* code)
{
    __try {
        return d->ProcessInput(0, b, flags, t, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return E_FAIL;
    }
}
HRESULT guardedOutput(IMediaObject* d, DMO_OUTPUT_DATA_BUFFER* ob, DWORD* status, unsigned long* code)
{
    __try {
        return d->ProcessOutput(0, 1, ob, status);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return E_FAIL;
    }
}

class DmoPlugin : public AudioPlugin {
public:
    struct Param {
        MP_TYPE type = MPT_FLOAT;
        MP_DATA lo = 0.f, hi = 1.f;
        std::string name, unit;
        std::vector<std::string> enumNames;
    };

    ComPtr<IMediaObject> dmo;
    ComPtr<IMediaObjectInPlace> inPlace;
    ComPtr<IMediaParams> params;
    ComPtr<IPersistStream> persist;
    std::vector<Param> plist;
    bool pages = false;
    PageHost editor;
    std::string stateAtOpen;

    std::mutex mutex;                   // process() against activate / deactivate / loadState
    bool active = false;
    PcmFormat fmt;
    int blockSize = 1024;
    int latency = 0;
    std::vector<BYTE> scratch;          // one block, interleaved (in-place processing)
    DxMediaBuffer* inBuf = nullptr;     // ProcessInput / ProcessOutput processing
    DxMediaBuffer* outBuf = nullptr;
    StereoFifo fifo;
    int64_t nextPos = 0;
    bool wasPlaying = false;
    std::atomic<bool> crashed{false};   // it crashed while processing: never called to process again

    bool noteCrash(unsigned long code)
    {
        if (!code) return false;
        if (!crashed.exchange(true)) logf("DirectX: %s crashed while processing (exception 0x%08lX): it is not used again", desc.name.c_str(), code);
        return true;
    }

    ~DmoPlugin() override
    {
        editor.close();
        deactivate();
        if (inBuf) inBuf->Release();
        if (outBuf) outBuf->Release();
    }

    bool init(const CLSID& clsid, std::string* error)
    {
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMediaObject), dmo.putVoid());
        if (FAILED(hr) || !dmo) { if (error) *error = "could not be created (" + hrText(FAILED(hr) ? hr : E_NOINTERFACE) + ")"; return false; }
        DWORD nIn = 0, nOut = 0;
        if (FAILED(dmo->GetStreamCount(&nIn, &nOut)) || nIn != 1 || nOut != 1) {
            if (error) *error = "is not an effect with one input and one output";
            return false;
        }
        queryInterface(dmo.get(), inPlace);
        queryInterface(dmo.get(), persist);
        readParams();
        pages = PageHost::hasPages(dmo.get());
        if (desc.name.empty()) {
            wchar_t name[80] = {0};
            if (SUCCEEDED(DMOGetName(clsid, name))) { name[79] = 0; desc.name = narrow(name); }
        }
        desc.hasEditor = pages;
        desc.numInputs = desc.numOutputs = 2;
        desc.isInstrument = false;
        return true;
    }

    void readParams()
    {
        ComPtr<IMediaParamInfo> info;
        if (!queryInterface(dmo.get(), info) || !queryInterface(dmo.get(), params)) { params.reset(); return; }
        DWORD count = 0;
        if (FAILED(info->GetParamCount(&count)) || count > 4096) count = 0;
        for (DWORD k = 0; k < count; ++k) {
            MP_PARAMINFO pi;
            std::memset(&pi, 0, sizeof(pi));
            if (FAILED(info->GetParamInfo(k, &pi))) break;          // indices must stay the DMO's own
            Param p;
            p.type = pi.mpType;
            p.lo = pi.mpdMinValue;
            p.hi = pi.mpdMaxValue;
            p.name = narrow(std::wstring(pi.szLabel, wcsnlen(pi.szLabel, 32)));
            p.unit = narrow(std::wstring(pi.szUnitText, wcsnlen(pi.szUnitText, 32)));
            // "Name\0Unit\0Enum1\0...EnumN\0\0": the names of an enumeration's values are only available here
            WCHAR* text = nullptr;
            if (SUCCEEDED(info->GetParamText(k, &text)) && text) {
                const WCHAR* s = text;
                for (int part = 0; *s && part < 258; ++part) {
                    const size_t len = wcsnlen(s, 1024);
                    if (len >= 1024) break;
                    const std::string str = narrow(std::wstring(s, len));
                    if (part == 0) { if (p.name.empty()) p.name = str; }
                    else if (part == 1) { if (p.unit.empty()) p.unit = str; }
                    else p.enumNames.push_back(str);
                    s += len + 1;
                }
                CoTaskMemFree(text);
            }
            if (p.name.empty()) p.name = strfmt("Parameter %u", (unsigned)k + 1);
            plist.push_back(p);
        }
    }

    // Input and output type of stream 0. The WAVEFORMATEX is ours; the DMO copies what it keeps.
    bool setTypes(const PcmFormat& f)
    {
        dmo->SetOutputType(0, nullptr, DMO_SET_TYPEF_CLEAR);
        dmo->SetInputType(0, nullptr, DMO_SET_TYPEF_CLEAR);
        WAVEFORMATEX wf;
        fillWave(wf, f);
        DMO_MEDIA_TYPE mt;
        std::memset(&mt, 0, sizeof(mt));
        mt.majortype = MEDIATYPE_Audio;
        mt.subtype = f.isFloat ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM;
        mt.bFixedSizeSamples = TRUE;
        mt.bTemporalCompression = FALSE;
        mt.lSampleSize = (ULONG)f.blockAlign();
        mt.formattype = FORMAT_WaveFormatEx;
        mt.cbFormat = sizeof(wf);
        mt.pbFormat = (BYTE*)&wf;
        if (dmo->SetInputType(0, &mt, 0) != S_OK) return false;
        if (dmo->SetOutputType(0, &mt, 0) != S_OK) { dmo->SetInputType(0, nullptr, DMO_SET_TYPEF_CLEAR); return false; }
        return true;
    }

    bool activate(double sr, int maxBlock) override
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!dmo || !comEnsure(false)) return false;
        if (active) { dmo->FreeStreamingResources(); active = false; }
        const int rate = (int)(sr + 0.5);
        if (rate < 1000) return false;
        blockSize = maxBlock < 16 ? 16 : maxBlock;
        PcmFormat f = stereoFormat(true, rate);
        if (!setTypes(f)) {
            f = stereoFormat(false, rate);
            if (!setTypes(f)) { logf("DirectX: %s accepts neither float nor 16-bit stereo at %d Hz", desc.name.c_str(), rate); return false; }
        }
        fmt = f;
        const HRESULT hr = dmo->AllocateStreamingResources();
        if (FAILED(hr) && hr != E_NOTIMPL) { logf("DirectX: %s could not start (%s)", desc.name.c_str(), hrText(hr).c_str()); return false; }
        const size_t blockBytes = (size_t)blockSize * (size_t)fmt.blockAlign();
        scratch.assign(blockBytes, 0);
        if (inBuf) inBuf->Release();
        if (outBuf) outBuf->Release();
        inBuf = outBuf = nullptr;
        latency = 0;
        if (inPlace) {
            REFERENCE_TIME t = 0;
            if (SUCCEEDED(inPlace->GetLatency(&t)) && t > 0 && t < 10 * (REFERENCE_TIME)10000000) latency = (int)(t * rate / 10000000);
        } else {
            DWORD outSize = 0, outAlign = 0;
            if (FAILED(dmo->GetOutputSizeInfo(0, &outSize, &outAlign)) || outSize > (DWORD)64 * 1024 * 1024) outSize = 0;
            inBuf = new DxMediaBuffer(blockBytes);
            outBuf = new DxMediaBuffer(std::max(blockBytes, (size_t)outSize));
        }
        fifo.setCapacity(fifoFramesFor(rate, blockSize));
        wasPlaying = false;
        active = true;
        return true;
    }

    void deactivate() override
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!dmo || !active) return;
        dmo->Flush();
        dmo->FreeStreamingResources();
        active = false;
    }

    // Everything the DMO has ready goes into the FIFO.
    void drainOutput()
    {
        for (int guard = 0; guard < 64; ++guard) {
            if (!outBuf->onlyOurs()) {                              // the DMO kept the last one: leave it to it
                const size_t cap = outBuf->capacity();
                outBuf->Release();
                outBuf = new DxMediaBuffer(cap);
            }
            outBuf->SetLength(0);
            DMO_OUTPUT_DATA_BUFFER ob;
            std::memset(&ob, 0, sizeof(ob));
            ob.pBuffer = outBuf;
            DWORD status = 0;
            unsigned long code = 0;
            const HRESULT hr = guardedOutput(dmo.get(), &ob, &status, &code);
            if (noteCrash(code) || FAILED(hr)) break;
            fifo.pushBytes(outBuf->bytes(), std::min((size_t)outBuf->length(), outBuf->capacity()), fmt);
            if (hr != S_OK || !(ob.dwStatus & DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE)) break;
        }
    }

    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override
    {
        std::unique_lock<std::mutex> lk(mutex, std::try_to_lock);
        if (!lk.owns_lock() || !dmo || !active || frames <= 0 || !left || !right || crashed.load()) return;
        if (!comEnsure(false)) return;
        if (playing && (!wasPlaying || samplePos != nextPos)) {     // playback (re)started elsewhere: no old echoes
            dmo->Flush();
            fifo.clear();
        }
        wasPlaying = playing;
        nextPos = samplePos + frames;
        const int step = fmt.blockAlign();
        int done = 0;
        while (done < frames) {
            const int n = std::min(blockSize, frames - done);
            const DWORD bytes = (DWORD)(n * step);
            const REFERENCE_TIME t = framesToTime(samplePos + done, fmt.rate);
            if (inPlace) {
                packStereo(left + done, right + done, n, fmt, scratch.data());
                unsigned long code = 0;
                const HRESULT hr = guardedInPlace(inPlace.get(), bytes, scratch.data(), t, &code);
                if (noteCrash(code)) return;
                if (SUCCEEDED(hr)) {
                    const BYTE* p = scratch.data();
                    for (int k = 0; k < n; ++k, p += step) readFrame(p, fmt, left[done + k], right[done + k]);
                }
            } else if (inBuf && outBuf) {
                if (!inBuf->onlyOurs()) {                           // the DMO still holds it from last time
                    const size_t cap = inBuf->capacity();
                    inBuf->Release();
                    inBuf = new DxMediaBuffer(cap);
                }
                packStereo(left + done, right + done, n, fmt, inBuf->bytes());
                inBuf->SetLength(bytes);
                const DWORD flags = DMO_INPUT_DATA_BUFFERF_SYNCPOINT | DMO_INPUT_DATA_BUFFERF_TIME;
                unsigned long code = 0;
                HRESULT hr = guardedInput(dmo.get(), inBuf, flags, t, &code);
                if (!code && hr == DMO_E_NOTACCEPTING) {            // it wants its output taken first
                    drainOutput();
                    hr = crashed.load() ? E_FAIL : guardedInput(dmo.get(), inBuf, flags, t, &code);
                }
                if (noteCrash(code) || crashed.load()) return;
                if (SUCCEEDED(hr)) drainOutput();
                if (crashed.load()) return;
                fifo.pop(left + done, right + done, (size_t)n);     // nothing ready: the block stays as it came
            }
            done += n;
        }
    }

    int latencySamples() override { return latency; }

    int paramCount() override { return params ? (int)plist.size() : 0; }
    bool validParam(int index) const { return params && index >= 0 && index < (int)plist.size(); }
    std::string paramName(int index) override { return validParam(index) ? plist[(size_t)index].name : std::string(); }

    MP_DATA rawGet(int index)
    {
        MP_DATA v = 0.f;
        if (FAILED(params->GetParam((DWORD)index, &v))) v = plist[(size_t)index].lo;
        return v;
    }

    std::string paramText(int index) override
    {
        if (!validParam(index)) return std::string();
        const Param& p = plist[(size_t)index];
        const MP_DATA v = rawGet(index);
        std::string text;
        switch (p.type) {
        case MPT_BOOL: text = v >= 0.5f ? "On" : "Off"; break;
        case MPT_ENUM: {
            const long k = std::lround(v - p.lo);
            if (k >= 0 && k < (long)p.enumNames.size()) text = p.enumNames[(size_t)k];
            else text = strfmt("%ld", std::lround(v));
            break;
        }
        case MPT_INT: text = strfmt("%ld", std::lround(v)); break;
        default: text = strfmt("%.2f", (double)v); break;
        }
        return p.unit.empty() ? text : text + " " + p.unit;
    }

    float paramGet(int index) override
    {
        if (!validParam(index)) return 0.f;
        const Param& p = plist[(size_t)index];
        const float range = p.hi - p.lo;
        if (!(range > 0.f)) return 0.f;
        const float x = (rawGet(index) - p.lo) / range;
        return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    }

    void paramSet(int index, float x) override
    {
        if (!validParam(index)) return;
        const Param& p = plist[(size_t)index];
        if (!(x == x)) return;
        x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
        MP_DATA v = p.lo + x * (p.hi - p.lo);
        if (p.type == MPT_BOOL) v = x >= 0.5f ? p.hi : p.lo;
        else if (p.type == MPT_INT || p.type == MPT_ENUM) v = std::floor(v + 0.5f);
        params->SetParam((DWORD)index, v);
    }

    // "S:" the DMO's own saved form, or "P:" its parameter values (as the DMO counts them, not 0..1).
    std::string saveState() override
    {
        if (!dmo || !comEnsure(false)) return std::string();
        std::string bytes;
        if (persist && persistSave(persist.get(), bytes)) return "S:" + base64Encode(bytes.data(), bytes.size());
        if (!params) return std::string();
        std::vector<MP_DATA> v(plist.size());
        for (size_t k = 0; k < plist.size(); ++k) v[k] = rawGet((int)k);
        return "P:" + base64Encode(v.data(), v.size() * sizeof(MP_DATA));
    }

    bool loadState(const std::string& s) override
    {
        if (!dmo || s.size() < 2 || s[1] != ':' || !comEnsure(false)) return false;
        const std::string raw = base64Decode(s.substr(2));
        if (s[0] == 'S' && persist) {
            std::lock_guard<std::mutex> lk(mutex);
            return persistLoad(persist.get(), raw);
        }
        if (s[0] == 'P' && params) {
            const size_t n = std::min(raw.size() / sizeof(MP_DATA), plist.size());
            for (size_t k = 0; k < n; ++k) {
                MP_DATA v;
                std::memcpy(&v, raw.data() + k * sizeof(MP_DATA), sizeof(MP_DATA));
                if (v == v) params->SetParam((DWORD)k, std::min(std::max(v, plist[k].lo), plist[k].hi));
            }
            return true;
        }
        return false;
    }

    bool hasEditor() override { return pages; }

    bool openEditor(void* parentWindow, int* w, int* h) override
    {
        if (!pages || !dmo || !comEnsure(true)) return false;
        if (!editor.open(dmo.get(), (HWND)parentWindow, w, h)) return false;
        stateAtOpen = saveState();
        return true;
    }

    void closeEditor() override
    {
        if (!editor.isOpen()) return;
        editor.applyIfDirty();
        editor.close();
        // a page may change its effect without saying so: the saved form shows it
        const bool changed = saveState() != stateAtOpen;
        stateAtOpen.clear();
        if (changed && onParamChangedByPlugin) onParamChangedByPlugin(-1);
    }

    void idle() override
    {
        if (editor.isOpen() && editor.applyIfDirty() && onParamChangedByPlugin) onParamChangedByPlugin(-1);
    }
};

// ================================================================================================ media types

void freeMediaType(AM_MEDIA_TYPE& mt)
{
    if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    mt.pbFormat = nullptr;
    mt.cbFormat = 0;
    if (mt.pUnk) mt.pUnk->Release();
    mt.pUnk = nullptr;
}

// A media type we were given whole (IMediaSample::GetMediaType).
void deleteMediaType(AM_MEDIA_TYPE* mt)
{
    if (!mt) return;
    freeMediaType(*mt);
    CoTaskMemFree(mt);
}

// 'dst' is raw memory. False when out of memory (dst is then empty).
bool copyMediaType(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src)
{
    dst = src;
    dst.pbFormat = nullptr;
    dst.cbFormat = 0;
    dst.pUnk = nullptr;
    if (src.cbFormat && src.pbFormat) {
        dst.pbFormat = (BYTE*)CoTaskMemAlloc(src.cbFormat);
        if (!dst.pbFormat) return false;
        std::memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
        dst.cbFormat = src.cbFormat;
    }
    dst.pUnk = src.pUnk;
    if (dst.pUnk) dst.pUnk->AddRef();
    return true;
}

// 'mt' is raw memory. The full audio type for 'f': a WAVEFORMATEX from CoTaskMemAlloc, as required.
bool makeAudioType(AM_MEDIA_TYPE& mt, const PcmFormat& f)
{
    std::memset(&mt, 0, sizeof(mt));
    WAVEFORMATEX* wf = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!wf) return false;
    fillWave(*wf, f);
    mt.majortype = MEDIATYPE_Audio;
    mt.subtype = f.isFloat ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = (ULONG)f.blockAlign();
    mt.formattype = FORMAT_WaveFormatEx;
    mt.pUnk = nullptr;
    mt.cbFormat = sizeof(WAVEFORMATEX);
    mt.pbFormat = (BYTE*)wf;
    return true;
}

// A media type that frees what it owns.
struct MediaType {
    AM_MEDIA_TYPE mt;
    MediaType() { std::memset(&mt, 0, sizeof(mt)); }
    ~MediaType() { freeMediaType(mt); }
    MediaType(const MediaType&) = delete;
    MediaType& operator=(const MediaType&) = delete;
    void clear() { freeMediaType(mt); std::memset(&mt, 0, sizeof(mt)); }
};

// The format of a complete audio media type, if it is one we can read.
bool formatOfType(const AM_MEDIA_TYPE& mt, PcmFormat& f)
{
    if (mt.majortype != MEDIATYPE_Audio || mt.formattype != FORMAT_WaveFormatEx) return false;
    return formatFromWave(mt.pbFormat, mt.cbFormat, f);
}

// Whether a possibly partial type (GUID_NULL = "any") leaves room for exactly 'f'.
bool typeAllows(const AM_MEDIA_TYPE& mt, const PcmFormat& f)
{
    if (mt.majortype != GUID_NULL && mt.majortype != MEDIATYPE_Audio) return false;
    if (mt.subtype != GUID_NULL && mt.subtype != (f.isFloat ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM)) return false;
    if (mt.formattype == GUID_NULL) return true;
    if (mt.formattype != FORMAT_WaveFormatEx) return false;
    if (!mt.pbFormat || mt.cbFormat == 0) return true;
    PcmFormat g;
    return formatFromWave(mt.pbFormat, mt.cbFormat, g) && g == f;
}

// The media types a pin of ours offers.
class DxEnumMediaTypes final : public IEnumMediaTypes {
public:
    DxEnumMediaTypes(const std::vector<PcmFormat>& formats, size_t pos) : formats_(formats), pos_(pos) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IEnumMediaTypes)) { *out = static_cast<IEnumMediaTypes*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = --refs_;
        if (n == 0) delete this;
        return (ULONG)n;
    }
    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override
    {
        if (fetched) *fetched = 0;
        if (!types) return E_POINTER;
        if (count != 1 && !fetched) return E_INVALIDARG;
        ULONG got = 0;
        while (got < count && pos_ < formats_.size()) {
            AM_MEDIA_TYPE* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            if (!mt) break;
            if (!makeAudioType(*mt, formats_[pos_])) { CoTaskMemFree(mt); break; }
            types[got++] = mt;
            ++pos_;
        }
        if (fetched) *fetched = got;
        return got == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override
    {
        const size_t left = formats_.size() - pos_;
        if (count > left) { pos_ = formats_.size(); return S_FALSE; }
        pos_ += count;
        return S_OK;
    }
    STDMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** out) override
    {
        if (!out) return E_POINTER;
        *out = new DxEnumMediaTypes(formats_, pos_);
        return S_OK;
    }

private:
    ~DxEnumMediaTypes() {}
    std::atomic<long> refs_{1};
    std::vector<PcmFormat> formats_;
    size_t pos_;
};

// ================================================================================================ our two filters

class DxPin;

// A filter with exactly one pin. With an output pin it is where our audio enters the graph; with an input pin it is
// where the effect's audio comes back. Both follow the DirectShow base classes' rules: connections change only
// while stopped, the output pin's allocator is committed while not stopped, and the owning graph is not
// reference counted.
class DxFilter final : public IBaseFilter {
public:
    explicit DxFilter(PIN_DIRECTION pinDirection);

    std::recursive_mutex lock;                  // connection and state (what m_pLock is in the base classes)
    std::atomic<int> state{State_Stopped};
    DxPin* pin = nullptr;                       // lives as long as the filter and shares its reference count

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IPersist) || iid == __uuidof(IMediaFilter) || iid == __uuidof(IBaseFilter)) {
            *out = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = --refs_;
        if (n == 0) delete this;
        return (ULONG)n;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* id) override
    {
        if (!id) return E_POINTER;
        *id = GUID_NULL;                        // not a registered class: only we can create one
        return S_OK;
    }

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME start) override;
    STDMETHODIMP GetState(DWORD, FILTER_STATE* out) override
    {
        if (!out) return E_POINTER;
        *out = (FILTER_STATE)state.load();
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* c) override
    {
        std::lock_guard<std::recursive_mutex> lk(lock);
        clock_.set(c);
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** out) override
    {
        if (!out) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(lock);
        *out = clock_.get();
        if (*out) (*out)->AddRef();
        return S_OK;
    }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** out) override;
    STDMETHODIMP FindPin(LPCWSTR id, IPin** out) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override
    {
        if (!info) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(lock);
        copyWide(info->achName, MAX_FILTER_NAME, name_.c_str());
        info->pGraph = graph_;
        if (info->pGraph) info->pGraph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override
    {
        std::lock_guard<std::recursive_mutex> lk(lock);
        graph_ = graph;                         // deliberately no AddRef: the graph holds us
        name_ = name ? name : L"";
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* out) override
    {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }

private:
    ~DxFilter();
    std::atomic<long> refs_{1};
    IFilterGraph* graph_ = nullptr;
    std::wstring name_;
    ComPtr<IReferenceClock> clock_;
};

// What the two kinds of pin have in common.
class DxPin : public IPin {
public:
    DxFilter* const filter;
    const PIN_DIRECTION dir;
    const wchar_t* const name;                  // also the pin's id
    std::atomic<long> generation{0};            // goes up every time the pin is connected or disconnected

    DxPin(DxFilter* f, PIN_DIRECTION d, const wchar_t* n) : filter(f), dir(d), name(n) {}
    virtual ~DxPin() {}

    // set by the plug-in while the pin is not connected
    void setFormat(const PcmFormat& f)
    {
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        format = f;
    }
    bool isConnected()
    {
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        return (bool)peer;
    }

    // called by the filter, with its lock held, when it leaves / enters the stopped state
    virtual HRESULT onActive() { return S_OK; }
    virtual void onInactive() {}

    STDMETHODIMP_(ULONG) AddRef() override { return filter->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return filter->Release(); }

    STDMETHODIMP Disconnect() override
    {
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        if (filter->state.load() != State_Stopped) return VFW_E_NOT_STOPPED;
        if (!peer) return S_FALSE;
        breakConnection();
        return S_OK;
    }
    STDMETHODIMP ConnectedTo(IPin** out) override
    {
        if (!out) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        *out = peer.get();
        if (!*out) return VFW_E_NOT_CONNECTED;
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* out) override
    {
        if (!out) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        if (!peer) { std::memset(out, 0, sizeof(*out)); return VFW_E_NOT_CONNECTED; }
        return copyMediaType(*out, connType.mt) ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override
    {
        if (!info) return E_POINTER;
        info->pFilter = filter;
        filter->AddRef();
        info->dir = dir;
        copyWide(info->achName, MAX_PIN_NAME, name);
        return S_OK;
    }
    STDMETHODIMP QueryDirection(PIN_DIRECTION* out) override
    {
        if (!out) return E_POINTER;
        *out = dir;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* out) override
    {
        if (!out) return E_POINTER;
        const size_t bytes = (std::wcslen(name) + 1) * sizeof(wchar_t);
        *out = (LPWSTR)CoTaskMemAlloc(bytes);
        if (!*out) return E_OUTOFMEMORY;
        std::memcpy(*out, name, bytes);
        return S_OK;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** out) override
    {
        if (!out) return E_POINTER;
        *out = new DxEnumMediaTypes(offeredFormats(), 0);
        return S_OK;
    }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

protected:
    // everything below: guarded by filter->lock
    ComPtr<IPin> peer;                          // the pin we are connected to
    MediaType connType;                         // the connection's media type
    PcmFormat format;                           // what we send (output pin) / the sample rate we insist on (input pin)

    virtual std::vector<PcmFormat> offeredFormats() = 0;
    virtual void breakConnection()
    {
        peer.reset();
        connType.clear();
        ++generation;
    }
};

// The output pin: hands our samples to the effect's input pin.
class DxOutPin final : public DxPin {
public:
    explicit DxOutPin(DxFilter* f) : DxPin(f, PINDIR_OUTPUT, L"Out") {}

    void setMaxBlock(int frames)
    {
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        maxBlock_ = frames;
    }

    // For process(): the connection cannot change while the filter is not stopped, and the plug-in only streams
    // while it is not, so these are read without the lock, as the base classes do.
    IMemInputPin* streamInput() const { return memInput_.get(); }
    IMemAllocator* streamAllocator() const { return allocator_.get(); }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IPin)) { *out = static_cast<IPin*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    // The graph manager calls this (ConnectDirect, and again when the effect asks to be reconnected).
    STDMETHODIMP Connect(IPin* receiver, const AM_MEDIA_TYPE* wanted) override
    {
        if (!receiver) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        if (peer) return VFW_E_ALREADY_CONNECTED;
        if (filter->state.load() != State_Stopped) return VFW_E_NOT_STOPPED;
        PIN_DIRECTION d = PINDIR_OUTPUT;
        if (FAILED(receiver->QueryDirection(&d)) || d != PINDIR_INPUT) return VFW_E_INVALID_DIRECTION;
        // only one type is sent; a type given by the caller (maybe partial) must leave room for it
        if (wanted && !typeAllows(*wanted, format)) return VFW_E_TYPE_NOT_ACCEPTED;
        // From here we count as connected, as a base-class pin does: the receiver may ask what we are connected to
        // and with what type while it decides. All of it is undone if it says no.
        connType.clear();
        if (!makeAudioType(connType.mt, format)) return E_OUTOFMEMORY;
        peer.set(receiver);
        HRESULT hr = receiver->ReceiveConnection(this, &connType.mt);
        if (SUCCEEDED(hr)) {
            if (!queryInterface(receiver, memInput_)) hr = VFW_E_NO_TRANSPORT;
            if (SUCCEEDED(hr)) hr = chooseAllocator(memInput_.get());
            if (FAILED(hr)) receiver->Disconnect();     // it had accepted: make it let go again
        }
        if (FAILED(hr)) {
            allocator_.reset();
            memInput_.reset();
            peer.reset();
            connType.clear();
            return hr;
        }
        ++generation;
        return S_OK;
    }
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override
    {
        if (!mt) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        return typeAllows(*mt, format) ? S_OK : S_FALSE;
    }
        // these travel downstream; nobody should send them to an output pin
    STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }

    HRESULT onActive() override { return allocator_ ? allocator_->Commit() : S_OK; }
    void onInactive() override { if (allocator_) allocator_->Decommit(); }

protected:
    std::vector<PcmFormat> offeredFormats() override
    {
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        return std::vector<PcmFormat>(1, format);
    }
    void breakConnection() override
    {
        if (allocator_) allocator_->Decommit();
        allocator_.reset();
        memInput_.reset();
        DxPin::breakConnection();
    }

private:
    // The allocator our samples come from: the system allocator sized for our blocks if the effect accepts it, else
    // whatever allocator the effect's pin offers.
    HRESULT chooseAllocator(IMemInputPin* mem)
    {
        const long blockBytes = (long)std::max(maxBlock_, 16) * (long)format.blockAlign();
        ALLOCATOR_PROPERTIES want;
        std::memset(&want, 0, sizeof(want));
        if (FAILED(mem->GetAllocatorRequirements(&want))) std::memset(&want, 0, sizeof(want));
        ALLOCATOR_PROPERTIES props;
        props.cBuffers = std::min(std::max(want.cBuffers, 8L), 64L);
        props.cbBuffer = std::min(std::max(want.cbBuffer, blockBytes), 64L * 1024 * 1024);
        props.cbAlign = (want.cbAlign > 1 && want.cbAlign <= 4096 && (want.cbAlign & (want.cbAlign - 1)) == 0) ? want.cbAlign : 1;
        props.cbPrefix = (want.cbPrefix > 0 && want.cbPrefix <= 4096) ? want.cbPrefix : 0;

        ComPtr<IMemAllocator> a;
        HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMemAllocator), a.putVoid());
        if (SUCCEEDED(hr) && !a) hr = E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = offerAllocator(mem, a.get(), props, blockBytes);
        if (FAILED(hr)) {
            a.reset();
            hr = mem->GetAllocator(a.put());
            if (SUCCEEDED(hr) && !a) hr = VFW_E_NO_ALLOCATOR;
            if (SUCCEEDED(hr)) hr = offerAllocator(mem, a.get(), props, blockBytes);
        }
        if (FAILED(hr)) return hr;
        allocator_ = a;
        return S_OK;
    }
    static HRESULT offerAllocator(IMemInputPin* mem, IMemAllocator* a, const ALLOCATOR_PROPERTIES& props, long blockBytes)
    {
        ALLOCATOR_PROPERTIES request = props, actual;
        std::memset(&actual, 0, sizeof(actual));
        HRESULT hr = a->SetProperties(&request, &actual);
        if (SUCCEEDED(hr) && (actual.cbBuffer < blockBytes || actual.cBuffers < 1)) hr = VFW_E_NO_ALLOCATOR;
        if (SUCCEEDED(hr)) hr = mem->NotifyAllocator(a, FALSE);
        return hr;
    }

    int maxBlock_ = 1024;
    ComPtr<IMemInputPin> memInput_;             // the effect's input pin, as the thing that takes samples
    ComPtr<IMemAllocator> allocator_;
};

// The input pin: collects what the effect delivers.
class DxInPin final : public DxPin, public IMemInputPin {
public:
    explicit DxInPin(DxFilter* f) : DxPin(f, PINDIR_INPUT, L"In") {}

    void setFifoFrames(size_t frames)
    {
        std::lock_guard<std::mutex> lk(dataLock_);
        fifo_.setCapacity(frames);
    }
    void clearFifo()
    {
        std::lock_guard<std::mutex> lk(dataLock_);
        fifo_.clear();
    }
    bool pop(float* left, float* right, int frames)
    {
        std::lock_guard<std::mutex> lk(dataLock_);
        return fifo_.pop(left, right, (size_t)frames);
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IPin)) { *out = static_cast<IPin*>(this); AddRef(); return S_OK; }
        if (iid == __uuidof(IMemInputPin)) { *out = static_cast<IMemInputPin*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return filter->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return filter->Release(); }

    // IPin
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) override
    {
        if (!connector || !mt) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        if (peer) return VFW_E_ALREADY_CONNECTED;
        if (filter->state.load() != State_Stopped) return VFW_E_NOT_STOPPED;
        PIN_DIRECTION d = PINDIR_INPUT;
        if (FAILED(connector->QueryDirection(&d)) || d != PINDIR_OUTPUT) return VFW_E_INVALID_DIRECTION;
        PcmFormat f;
        if (!accepts(*mt, f)) return VFW_E_TYPE_NOT_ACCEPTED;
        connType.clear();
        if (!copyMediaType(connType.mt, *mt)) return E_OUTOFMEMORY;
        peer.set(connector);
        {
            std::lock_guard<std::mutex> dl(dataLock_);
            received_ = f;
        }
        flushing_ = false;
        ++generation;
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override
    {
        if (!mt) return E_POINTER;
        PcmFormat f;
        return accepts(*mt, f) ? S_OK : S_FALSE;
    }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { flushing_ = true; return S_OK; }
    STDMETHODIMP EndFlush() override { flushing_ = false; return S_OK; }

    // IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return VFW_E_NO_ALLOCATOR;              // the effect's output pin brings its own
    }
    STDMETHODIMP NotifyAllocator(IMemAllocator* a, BOOL) override { return a ? S_OK : E_POINTER; }     // any will do: we only read
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }
    STDMETHODIMP Receive(IMediaSample* sample) override
    {
        if (!sample) return E_POINTER;
        if (filter->state.load() == State_Stopped) return VFW_E_WRONG_STATE;
        if (flushing_.load()) return S_FALSE;
        // a sample may carry a new format for itself and everything after it
        bool changed = false;
        PcmFormat f;
        AM_MEDIA_TYPE* mt = nullptr;
        if (sample->GetMediaType(&mt) == S_OK && mt) {
            const bool ok = accepts(*mt, f);
            deleteMediaType(mt);
            if (!ok) return VFW_E_TYPE_NOT_ACCEPTED;
            changed = true;
        }
        BYTE* data = nullptr;
        const HRESULT hr = sample->GetPointer(&data);
        const long bytes = sample->GetActualDataLength();
        std::lock_guard<std::mutex> lk(dataLock_);
        if (changed) received_ = f;
        if (SUCCEEDED(hr) && data && bytes > 0) fifo_.pushBytes(data, (size_t)bytes, received_);
        return S_OK;
    }
    STDMETHODIMP ReceiveMultiple(IMediaSample** samples, long count, long* processed) override
    {
        if (!samples || !processed) return E_POINTER;
        *processed = 0;
        for (long k = 0; k < count; ++k) {
            const HRESULT hr = Receive(samples[k]);
            if (hr != S_OK) return hr;
            ++*processed;
        }
        return S_OK;
    }
    STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

protected:
    std::vector<PcmFormat> offeredFormats() override
    {
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        std::vector<PcmFormat> v(1, format);
        if (format.isFloat) v.push_back(stereoFormat(false, format.rate));
        return v;
    }

private:
    // Plain PCM or float audio at our sample rate, whatever the width and channel count.
    bool accepts(const AM_MEDIA_TYPE& mt, PcmFormat& f)
    {
        if (!formatOfType(mt, f)) return false;
        std::lock_guard<std::recursive_mutex> lk(filter->lock);
        return f.rate == format.rate;
    }

    std::atomic<bool> flushing_{false};
    std::mutex dataLock_;                       // the two below; never held while calling out
    StereoFifo fifo_;
    PcmFormat received_;                        // the format of what arrives
};

// The pins of a DxFilter: there is one.
class DxEnumPins final : public IEnumPins {
public:
    DxEnumPins(DxPin* pin, int pos) : pos_(pos) { pin_.set(pin); }      // holding the pin holds its filter

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IEnumPins)) { *out = static_cast<IEnumPins*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = --refs_;
        if (n == 0) delete this;
        return (ULONG)n;
    }
    STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override
    {
        if (fetched) *fetched = 0;
        if (!pins) return E_POINTER;
        if (count != 1 && !fetched) return E_INVALIDARG;
        ULONG got = 0;
        if (count >= 1 && pos_ == 0) {
            pins[0] = pin_.get();
            pins[0]->AddRef();
            pos_ = 1;
            got = 1;
        }
        if (fetched) *fetched = got;
        return got == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override
    {
        if (count == 0) return S_OK;
        const bool enough = pos_ == 0 && count == 1;
        pos_ = 1;
        return enough ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** out) override
    {
        if (!out) return E_POINTER;
        *out = new DxEnumPins(pin_.get(), pos_);
        return S_OK;
    }

private:
    ~DxEnumPins() {}
    std::atomic<long> refs_{1};
    ComPtr<DxPin> pin_;
    int pos_;
};

DxFilter::DxFilter(PIN_DIRECTION pinDirection)
{
    if (pinDirection == PINDIR_OUTPUT) pin = new DxOutPin(this);
    else pin = new DxInPin(this);
}

DxFilter::~DxFilter()
{
    delete pin;                                 // lets go of the pin it was connected to, if it still was
}

STDMETHODIMP DxFilter::Stop()
{
    std::lock_guard<std::recursive_mutex> lk(lock);
    if (state.load() != State_Stopped) {
        pin->onInactive();
        state = State_Stopped;
    }
    return S_OK;
}

STDMETHODIMP DxFilter::Pause()
{
    std::lock_guard<std::recursive_mutex> lk(lock);
    if (state.load() == State_Stopped) {
        const HRESULT hr = pin->onActive();
        if (FAILED(hr)) return hr;
    }
    state = State_Paused;
    return S_OK;
}

STDMETHODIMP DxFilter::Run(REFERENCE_TIME)
{
    std::lock_guard<std::recursive_mutex> lk(lock);
    if (state.load() == State_Stopped) {
        const HRESULT hr = pin->onActive();
        if (FAILED(hr)) return hr;
    }
    state = State_Running;
    return S_OK;
}

STDMETHODIMP DxFilter::EnumPins(IEnumPins** out)
{
    if (!out) return E_POINTER;
    *out = new DxEnumPins(pin, 0);
    return S_OK;
}

STDMETHODIMP DxFilter::FindPin(LPCWSTR id, IPin** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!id) return E_POINTER;
    if (std::wcscmp(id, pin->name) != 0) return VFW_E_NOT_FOUND;
    *out = pin;
    pin->AddRef();
    return S_OK;
}

// ================================================================================================ DirectShow effects

// A filter's pins of one direction that are not connected to anything (a few at most).
std::vector<ComPtr<IPin>> freePins(IBaseFilter* f, PIN_DIRECTION dir)
{
    std::vector<ComPtr<IPin>> out;
    ComPtr<IEnumPins> e;
    if (!f || FAILED(f->EnumPins(e.put())) || !e) return out;
    for (int guard = 0; guard < 64 && out.size() < 8; ++guard) {
        ComPtr<IPin> p;
        ULONG got = 0;
        if (e->Next(1, p.put(), &got) != S_OK || !p) break;
        PIN_DIRECTION d = PINDIR_INPUT;
        if (FAILED(p->QueryDirection(&d)) || d != dir) continue;
        ComPtr<IPin> other;
        if (SUCCEEDED(p->ConnectedTo(other.put())) && other) continue;
        out.push_back(p);
    }
    return out;
}

class DsPlugin : public AudioPlugin {
public:
    ComPtr<IBaseFilter> fx;
    ComPtr<IPersistStream> persist;
    bool pages = false;
    PageHost editor;
    std::string stateAtOpen;

    std::mutex mutex;                   // process() against activate / deactivate / loadState
    // the graph (all guarded by 'mutex')
    ComPtr<IFilterGraph> graph;
    ComPtr<IMediaControl> control;
    ComPtr<DxFilter> source, sink;
    DxOutPin* srcPin = nullptr;         // owned by 'source'
    DxInPin* sinkPin = nullptr;         // owned by 'sink'
    ComPtr<IPin> fxIn, fxOut;
    PcmFormat fmt;                      // what we send the effect
    int graphBlock = 0;                 // frames per sample, at most
    bool built = false, running = false;
    // streaming
    int64_t streamFrames = 0;           // frames sent since the graph was started (sample times count from there)
    int64_t nextPos = 0;
    bool wasPlaying = false, discontinuity = true;

    ~DsPlugin() override
    {
        editor.close();
        std::lock_guard<std::mutex> lk(mutex);
        teardownGraph();
        persist.reset();
        fx.reset();
    }

    bool init(const CLSID& clsid, std::string* error)
    {
        const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IBaseFilter), fx.putVoid());
        if (FAILED(hr) || !fx) { if (error) *error = "could not be created (" + hrText(FAILED(hr) ? hr : E_NOINTERFACE) + ")"; return false; }
        queryInterface(fx.get(), persist);
        pages = PageHost::hasPages(fx.get());
        desc.hasEditor = pages;
        desc.numInputs = desc.numOutputs = 2;
        desc.isInstrument = false;
        // Connect now at a usual rate: an effect that cannot be connected is no use and should fail here, and a
        // property page expects an effect that is in a graph. activate() reconnects for the real rate.
        std::lock_guard<std::mutex> lk(mutex);
        std::string why, ignored;
        if (!buildGraph(48000, 1024, &why) && !buildGraph(44100, 1024, &ignored)) {
            if (error) *error = "could not be connected as a stereo audio effect (" + why + ")";
            return false;
        }
        return true;
    }

    // ---- the graph. Callers hold 'mutex'.

    void teardownGraph()
    {
        if (control) control->Stop();
        running = false;
        built = false;
        if (graph) {
            if (fxOut) graph->Disconnect(fxOut.get());
            if (sinkPin) graph->Disconnect(sinkPin);
            if (srcPin) graph->Disconnect(srcPin);
            if (fxIn) graph->Disconnect(fxIn.get());
            if (sink) graph->RemoveFilter(sink.get());
            if (fx) graph->RemoveFilter(fx.get());
            if (source) graph->RemoveFilter(source.get());
        }
        fxIn.reset();
        fxOut.reset();
        control.reset();
        graph.reset();
        srcPin = nullptr;
        sinkPin = nullptr;
        source.reset();
        sink.reset();
    }

    // source -> effect -> sink in a new graph, for 'rate' and blocks of at most 'maxBlock' frames. Left stopped.
    bool buildGraph(int rate, int maxBlock, std::string* why)
    {
        teardownGraph();
        HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFilterGraph), graph.putVoid());
        if (FAILED(hr) || !graph) { if (why) *why = "DirectShow is not available: " + hrText(FAILED(hr) ? hr : E_NOINTERFACE); teardownGraph(); return false; }
        if (!queryInterface(graph.get(), control)) { if (why) *why = "DirectShow is not available"; teardownGraph(); return false; }
        ComPtr<IMediaFilter> graphFilter;
        if (queryInterface(graph.get(), graphFilter)) graphFilter->SetSyncSource(nullptr);       // no clock: samples go as fast as we send them
        source.attach(new DxFilter(PINDIR_OUTPUT));
        sink.attach(new DxFilter(PINDIR_INPUT));
        srcPin = static_cast<DxOutPin*>(source->pin);
        sinkPin = static_cast<DxInPin*>(sink->pin);
        std::wstring fxName = widen(desc.name);
        if (fxName.empty()) fxName = L"Effect";
        if (fxName.size() >= MAX_FILTER_NAME) fxName.resize(MAX_FILTER_NAME - 1);
        hr = graph->AddFilter(source.get(), L"Reno In");
        if (SUCCEEDED(hr)) hr = graph->AddFilter(fx.get(), fxName.c_str());
        if (SUCCEEDED(hr)) hr = graph->AddFilter(sink.get(), L"Reno Out");
        if (FAILED(hr)) { if (why) *why = "it would not join a filter graph: " + hrText(hr); teardownGraph(); return false; }

        HRESULT lastError = VFW_E_NO_ACCEPTABLE_TYPES;
        bool sawInput = false, sawOutput = false;
        for (int attempt = 0; attempt < 2 && !built; ++attempt) {
            const PcmFormat f = stereoFormat(attempt == 0, rate);
            srcPin->setFormat(f);
            srcPin->setMaxBlock(maxBlock);
            sinkPin->setFormat(f);
            sinkPin->setFifoFrames(fifoFramesFor(rate, maxBlock));
            MediaType mt;
            if (!makeAudioType(mt.mt, f)) { lastError = E_OUTOFMEMORY; break; }

            // our output -> the effect's input
            for (auto& p : freePins(fx.get(), PINDIR_INPUT)) {
                sawInput = true;
                hr = graph->ConnectDirect(srcPin, p.get(), &mt.mt);
                if (SUCCEEDED(hr)) { fxIn = p; break; }
                lastError = hr;
            }
            if (!fxIn) continue;

            // the effect's output -> our input. The same type first; if the effect wants to output something else,
            // whatever it proposes that the input pin can read.
            for (auto& p : freePins(fx.get(), PINDIR_OUTPUT)) {
                sawOutput = true;
                hr = graph->ConnectDirect(p.get(), sinkPin, &mt.mt);
                if (FAILED(hr)) hr = graph->ConnectDirect(p.get(), sinkPin, nullptr);
                if (SUCCEEDED(hr)) { fxOut = p; break; }
                lastError = hr;
            }
            if (!fxOut) {
                graph->Disconnect(srcPin);
                graph->Disconnect(fxIn.get());
                fxIn.reset();
                continue;
            }

            // An in-place transform now asks the graph to reconnect its input so both sides can share an allocator;
            // the graph does that a moment later on its own thread: it disconnects our output pin and calls its
            // Connect again. Wait until nothing has moved for a little while so the reconnection is not cut short
            // by the graph starting.
            settleConnections();
            if (!srcPin->isConnected()) {               // a reconnection fell through: restore what was there
                hr = graph->ConnectDirect(srcPin, fxIn.get(), &mt.mt);
                if (FAILED(hr)) lastError = hr;
            }
            if (!sinkPin->isConnected()) {
                hr = graph->ConnectDirect(fxOut.get(), sinkPin, &mt.mt);
                if (FAILED(hr)) hr = graph->ConnectDirect(fxOut.get(), sinkPin, nullptr);
                if (FAILED(hr)) lastError = hr;
            }
            if (!srcPin->isConnected() || !sinkPin->isConnected()) {
                graph->Disconnect(fxOut.get());
                graph->Disconnect(sinkPin);
                graph->Disconnect(srcPin);
                graph->Disconnect(fxIn.get());
                fxIn.reset();
                fxOut.reset();
                continue;
            }
            fmt = f;
            graphBlock = maxBlock;
            built = true;
        }
        if (!built) {
            if (why) {
                if (!sawInput) *why = "it has no free input pin";
                else if (fxIn || sawOutput || lastError != VFW_E_NO_ACCEPTABLE_TYPES) *why = hrText(lastError);
                else *why = "it has no output pin";
            }
            teardownGraph();
            return false;
        }
        return true;
    }

    // Waits (at most 2 seconds) until our two pins have stayed connected, untouched, for 60 ms. Gives up sooner on a
    // pin that was disconnected and not reconnected within half a second.
    void settleConnections()
    {
        const ULONGLONG start = GetTickCount64();
        ULONGLONG lastChange = start;
        long srcGen = srcPin->generation.load(), sinkGen = sinkPin->generation.load();
        for (;;) {
            const bool connected = srcPin->isConnected() && sinkPin->isConnected();     // also waits out a Connect in progress
            const ULONGLONG now = GetTickCount64();
            const long a = srcPin->generation.load(), b = sinkPin->generation.load();
            if (a != srcGen || b != sinkGen) { srcGen = a; sinkGen = b; lastChange = now; }
            if (now - start >= 2000) break;
            if (now - lastChange >= (connected ? 60u : 500u)) break;
            Sleep(1);
        }
    }

    bool startGraph()
    {
        if (!built || !control) return false;
        HRESULT hr = control->Run();
        if (hr == S_FALSE) {                        // still on its way there
            OAFilterState st = 0;
            hr = control->GetState(2000, &st);
        }
        if (SUCCEEDED(hr) && (!srcPin->isConnected() || !sinkPin->isConnected() || !srcPin->streamInput() || !srcPin->streamAllocator())) hr = VFW_E_NOT_CONNECTED;
        if (FAILED(hr)) {
            control->Stop();
            logf("DirectX: %s could not be started (%s)", desc.name.c_str(), hrText(hr).c_str());
            return false;
        }
        if (fxIn) fxIn->NewSegment(0, 0x7FFFFFFFFFFFFFFFLL, 1.0);      // what a source sends before its first sample
        sinkPin->clearFifo();
        streamFrames = 0;
        discontinuity = true;
        wasPlaying = false;
        running = true;
        return true;
    }

    void stopGraph()
    {
        if (control) control->Stop();
        running = false;
    }

    // ---- AudioPlugin

    bool activate(double sr, int maxBlock) override
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!fx || !comEnsure(false)) return false;
        stopGraph();
        const int rate = (int)(sr + 0.5);
        if (rate < 1000) return false;
        const int block = maxBlock < 16 ? 16 : maxBlock;
        if (!built || rate != fmt.rate || block > graphBlock) {
            std::string why;
            if (!buildGraph(rate, block, &why)) {
                logf("DirectX: %s cannot run at %d Hz (%s)", desc.name.c_str(), rate, why.c_str());
                return false;
            }
        }
        return startGraph();
    }

    void deactivate() override
    {
        std::lock_guard<std::mutex> lk(mutex);
        stopGraph();
    }

    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override
    {
        std::unique_lock<std::mutex> lk(mutex, std::try_to_lock);
        if (!lk.owns_lock() || !running || frames <= 0 || !left || !right) return;
        if (!comEnsure(false)) return;
        IMemInputPin* const input = srcPin->streamInput();
        IMemAllocator* const allocator = srcPin->streamAllocator();
        if (!input || !allocator) return;
        if (playing && (!wasPlaying || samplePos != nextPos)) {     // playback (re)started elsewhere
            discontinuity = true;
            sinkPin->clearFifo();
        }
        wasPlaying = playing;
        nextPos = samplePos + frames;
        const int step = fmt.blockAlign();
        int done = 0;
        while (done < frames) {
            const int n = std::min(graphBlock, frames - done);
            const long bytes = (long)n * step;
            IMediaSample* sample = nullptr;
            // never wait for a buffer on the audio thread: an effect holding all of them costs a dry block
            if (SUCCEEDED(allocator->GetBuffer(&sample, nullptr, nullptr, AM_GBF_NOWAIT)) && sample) {
                BYTE* data = nullptr;
                if (SUCCEEDED(sample->GetPointer(&data)) && data && sample->GetSize() >= bytes) {
                    packStereo(left + done, right + done, n, fmt, data);
                    REFERENCE_TIME t0 = framesToTime(streamFrames, fmt.rate), t1 = framesToTime(streamFrames + n, fmt.rate);
                    sample->SetActualDataLength(bytes);
                    sample->SetTime(&t0, &t1);
                    sample->SetMediaTime(nullptr, nullptr);
                    sample->SetSyncPoint(TRUE);
                    sample->SetPreroll(FALSE);
                    sample->SetDiscontinuity(discontinuity ? TRUE : FALSE);
                    sample->SetMediaType(nullptr);
                    discontinuity = false;
                    input->Receive(sample);         // most effects deliver to our input pin before this returns
                }
                sample->Release();
            }
            streamFrames += n;
            // an effect that has not delivered (yet): the block stays as it came
            sinkPin->pop(left + done, right + done, n);
            done += n;
        }
    }

    // "S:" the filter's own saved form (IPersistStream). A filter without one has nothing to store.
    std::string saveState() override
    {
        if (!persist || !comEnsure(false)) return std::string();
        std::string bytes;
        if (!persistSave(persist.get(), bytes)) return std::string();
        return "S:" + base64Encode(bytes.data(), bytes.size());
    }

    bool loadState(const std::string& s) override
    {
        if (!persist || s.size() < 2 || s[0] != 'S' || s[1] != ':' || !comEnsure(false)) return false;
        const std::string raw = base64Decode(s.substr(2));
        std::lock_guard<std::mutex> lk(mutex);
        if (persistLoad(persist.get(), raw)) return true;
        // Older Reno versions read VEGAS's plug-in settings from a .veg without their first four bytes (the length their
        // saved form starts with), which those plug-ins refuse; projects or auto-saves from then get the settings back
        // only for a plug-in whose own saved form starts with its length
        std::string mine;
        if (raw.size() >= 20 && persistSave(persist.get(), mine) && mine.size() >= 4 && raw.size() + 4 == mine.size()) {
            uint32_t n = 0;
            std::memcpy(&n, mine.data(), 4);
            if (n == mine.size()) {
                std::string whole(4, '\0');
                n = (uint32_t)(raw.size() + 4);
                std::memcpy(&whole[0], &n, 4);
                return persistLoad(persist.get(), whole + raw);
            }
        }
        return false;
    }

    bool hasEditor() override { return pages; }

    bool openEditor(void* parentWindow, int* w, int* h) override
    {
        if (!pages || !fx || !comEnsure(true)) return false;
        if (!editor.open(fx.get(), (HWND)parentWindow, w, h)) return false;
        stateAtOpen = saveState();
        return true;
    }

    void closeEditor() override
    {
        if (!editor.isOpen()) return;
        editor.applyIfDirty();
        editor.close();
        // most pages change their filter as the user moves things, without telling anybody: the saved form shows it
        const bool changed = saveState() != stateAtOpen;
        stateAtOpen.clear();
        if (changed && onParamChangedByPlugin) onParamChangedByPlugin(-1);
    }

    void idle() override
    {
        if (editor.isOpen() && editor.applyIfDirty() && onParamChangedByPlugin) onParamChangedByPlugin(-1);
    }
};

// ================================================================================================ listing

const char* const kDmoPrefix = "dmo:";
const char* const kDshowPrefix = "dshow:";

bool lessByName(const AudioPluginDesc& a, const AudioPluginDesc& b) { return toLower(a.name) < toLower(b.name); }

AudioPluginDesc dxDesc(const char* prefix, const GUID& clsid, const std::string& name, bool editor)
{
    AudioPluginDesc d;
    d.format = "DX";
    d.uid = prefix + guidText(clsid);
    d.name = name;
    d.numInputs = d.numOutputs = 2;
    d.hasEditor = editor;
    return d;
}

// The DMOs registered as audio effects. Nothing is loaded: names and class ids come from the registry.
void listDmoEffects(std::vector<AudioPluginDesc>& out, std::set<std::string>& seen)
{
    // The sample rate converter is also registered as an audio effect; between two equal formats it does nothing.
    static const GUID kResampler = { 0xf447b69e, 0x1884, 0x4a7e, { 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3 } };
    ComPtr<IEnumDMO> e;
    if (FAILED(DMOEnum(DMOCATEGORY_AUDIO_EFFECT, 0, 0, nullptr, 0, nullptr, e.put())) || !e) return;
    std::vector<AudioPluginDesc> found;
    for (int guard = 0; guard < 4096; ++guard) {
        CLSID clsid = GUID_NULL;
        WCHAR* name = nullptr;
        DWORD got = 0;
        if (e->Next(1, &clsid, &name, &got) != S_OK || got != 1) { if (name) CoTaskMemFree(name); break; }
        std::string n = name ? narrow(name) : std::string();
        if (name) CoTaskMemFree(name);
        if (clsid == kResampler || toLower(n).find("resampl") != std::string::npos) continue;
        if (n.empty()) n = guidText(clsid);
        if (!seen.insert(toLower(guidText(clsid))).second) continue;
        // property pages are only known once it is loaded; its parameters can be shown without
        found.push_back(dxDesc(kDmoPrefix, clsid, n, false));
    }
    std::sort(found.begin(), found.end(), lessByName);
    out.insert(out.end(), found.begin(), found.end());
}

// A pin as a filter's registration describes it.
struct RegPin {
    bool output = false;
    std::vector<std::pair<GUID, GUID>> types;       // major type, subtype
};

// Reads "FilterData", the packed REGFILTER2 (version 2) the filter mapper keeps in the registry: {version, merit,
// pin count, reserved}, then for each pin {"0pi3", flags, instances, type count, medium count, has-category} [and
// the pin category offset], one {"0ty3", reserved, major type offset, subtype offset} per media type and one offset
// per medium. Offsets are from the start and point at GUIDs. False for anything that does not fit.
bool parseFilterData(const std::vector<BYTE>& d, std::vector<RegPin>& pins)
{
    auto u32 = [&](size_t off, uint32_t& v) {
        if (off > d.size() || d.size() - off < 4) return false;
        std::memcpy(&v, d.data() + off, 4);
        return true;
    };
    auto guid = [&](uint32_t off, GUID& g) {
        if (off > d.size() || d.size() - off < sizeof(GUID)) return false;
        std::memcpy(&g, d.data() + off, sizeof(GUID));
        return true;
    };
    uint32_t version = 0, count = 0;
    if (!u32(0, version) || !u32(8, count) || version != 2 || count > 64) return false;
    size_t pos = 16;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t flags = 0, nTypes = 0, nMediums = 0, hasCategory = 0;
        if (!u32(pos + 4, flags) || !u32(pos + 12, nTypes) || !u32(pos + 16, nMediums) || !u32(pos + 20, hasCategory)) return false;
        if (nTypes > 512 || nMediums > 512) return false;
        pos += 24 + (hasCategory ? 4 : 0);
        RegPin pin;
        pin.output = (flags & REG_PINFLAG_B_OUTPUT) != 0;
        for (uint32_t t = 0; t < nTypes; ++t) {
            uint32_t offMajor = 0, offSub = 0;
            if (!u32(pos + 8, offMajor) || !u32(pos + 12, offSub)) return false;
            GUID major = GUID_NULL, sub = GUID_NULL;
            if (!guid(offMajor, major) || !guid(offSub, sub)) return false;
            pin.types.push_back(std::make_pair(major, sub));
            pos += 16;
        }
        pos += (size_t)nMediums * 4;
        if (pos > d.size()) return false;
        pins.push_back(pin);
    }
    return true;
}

bool bagString(IPropertyBag* bag, const wchar_t* key, std::wstring& out)
{
    VARIANT v;
    VariantInit(&v);
    const bool ok = SUCCEEDED(bag->Read(key, &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal;
    if (ok) out.assign(v.bstrVal, SysStringLen(v.bstrVal));
    VariantClear(&v);
    return ok;
}

bool bagBytes(IPropertyBag* bag, const wchar_t* key, std::vector<BYTE>& out)
{
    VARIANT v;
    VariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(bag->Read(key, &v, nullptr)) && v.vt == (VT_ARRAY | VT_UI1) && v.parray && SafeArrayGetDim(v.parray) == 1) {
        LONG lo = 0, hi = -1;
        void* data = nullptr;
        if (SUCCEEDED(SafeArrayGetLBound(v.parray, 1, &lo)) && SUCCEEDED(SafeArrayGetUBound(v.parray, 1, &hi)) && hi >= lo &&
            SUCCEEDED(SafeArrayAccessData(v.parray, &data))) {
            if (data) { out.assign((const BYTE*)data, (const BYTE*)data + (size_t)(hi - lo + 1)); ok = true; }
            SafeArrayUnaccessData(v.parray);
        }
    }
    VariantClear(&v);
    return ok;
}

// Names that say "I am plumbing, not an effect".
bool nameIsNotAnEffect(const std::string& name)
{
    static const char* const kParts[] = { "decod", "encod", "decompress", "splitter", "demux", "muxer", "multiplex", "parser",
                                          "render", "codec", "grabber", "resampl" };
    static const char* const kWords[] = { "mux", "tee", "dump", "null" };
    const std::string s = toLower(name);
    size_t k = 0;
    while (k < s.size()) {
        while (k < s.size() && !std::isalnum((unsigned char)s[k])) ++k;
        size_t e = k;
        while (e < s.size() && std::isalnum((unsigned char)s[e])) ++e;
        if (e > k) {
            const std::string word = s.substr(k, e - k);
            for (const char* w : kWords) if (word == w) return true;
            for (const char* p : kParts) if (word.find(p) != std::string::npos) return true;
        }
        k = e;
    }
    return false;
}

// DirectShow filters that could be audio effects, judged by what their registration says about their pins. No
// filter is created; the filter mapper answers from the registry. Microsoft documents no filter category for audio
// effects (DirectX plug-ins register as ordinary DirectShow filters), so the query is "filters with an input pin and
// an output pin that both take uncompressed audio", in any category; well-known non-effects are dropped afterwards.
void listDirectShowEffects(std::vector<AudioPluginDesc>& out, std::set<std::string>& seen)
{
    static const GUID kSampleGrabber = { 0xc1f400a0, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
    static const GUID kNullRenderer = { 0xc1f400a4, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
    static const GUID kMp3Decoder = { 0x38be3000, 0xdbf4, 0x11d0, { 0x86, 0x0e, 0x00, 0xa0, 0x24, 0xcf, 0xef, 0x6d } };
    // Microsoft's own filters that match the question without being effects
    const GUID* const skipFilters[] = {
        &CLSID_ACMWrapper, &CLSID_InfTee, &CLSID_SmartTee, &CLSID_DMOWrapperFilter, &CLSID_AudioRender, &CLSID_DSoundRender,
        &CLSID_CMpegAudioCodec, &CLSID_MPEG1Splitter, &CLSID_AviSplitter, &CLSID_AviDest, &CLSID_DVMux, &CLSID_DVSplitter,
        &CLSID_MMSPLITTER, &CLSID_WMAsfWriter, &CLSID_FileWriter, &kSampleGrabber, &kNullRenderer, &kMp3Decoder
    };
    // categories whose members are codecs and devices
    const GUID* const skipCategories[] = {
        &CLSID_AudioCompressorCategory, &CLSID_VideoCompressorCategory, &CLSID_AudioRendererCategory, &CLSID_MidiRendererCategory,
        &CLSID_AudioInputDeviceCategory, &CLSID_VideoInputDeviceCategory, &CLSID_TransmitCategory, &CLSID_DeviceControlCategory,
        &CLSID_MediaEncoderCategory, &CLSID_MediaMultiplexerCategory, &CLSID_DVDHWDecodersCategory
    };

    ComPtr<IFilterMapper2> mapper;
    if (FAILED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFilterMapper2), mapper.putVoid())) || !mapper) return;
    // (major type, subtype) pairs; a pin registered with "any" in either place matches as well
    const GUID types[4] = { MEDIATYPE_Audio, MEDIASUBTYPE_PCM, MEDIATYPE_Audio, MEDIASUBTYPE_IEEE_FLOAT };
    ComPtr<IEnumMoniker> e;
    const HRESULT hr = mapper->EnumMatchingFilters(e.put(), 0, FALSE, 0,
                                                   TRUE, 2, types, nullptr, nullptr,
                                                   FALSE,
                                                   TRUE, 2, types, nullptr, nullptr);
    if (FAILED(hr) || !e) return;
    ComPtr<IBindCtx> bind;
    if (FAILED(CreateBindCtx(0, bind.put()))) bind.reset();

    std::vector<AudioPluginDesc> found;
    for (int guard = 0; guard < 20000; ++guard) {
        ComPtr<IMoniker> m;
        ULONG got = 0;
        if (e->Next(1, m.put(), &got) != S_OK || !m) break;

        // "@device:sw:{category}\{filter}" is a filter in a DLL. Anything else is a codec, a DMO in a wrapper or a
        // driver ("@device:cm:", "@device:dmo:", "@device:pnp:").
        LPOLESTR display = nullptr;
        if (SUCCEEDED(m->GetDisplayName(bind.get(), nullptr, &display)) && display) {
            const std::wstring dn(display);
            CoTaskMemFree(display);
            static const wchar_t kSoftware[] = L"@device:sw:";
            const size_t prefix = std::wcslen(kSoftware);
            if (dn.compare(0, prefix, kSoftware) != 0) continue;
            GUID category = GUID_NULL;
            bool skip = false;
            if (dn.size() >= prefix + 38 && guidParse(dn.substr(prefix, 38), category))
                for (const GUID* c : skipCategories) if (category == *c) skip = true;
            if (skip) continue;
        }

        ComPtr<IPropertyBag> bag;
        if (FAILED(m->BindToStorage(nullptr, nullptr, __uuidof(IPropertyBag), bag.putVoid())) || !bag) continue;
        std::wstring clsidText, friendly;
        GUID clsid = GUID_NULL;
        if (!bagString(bag.get(), L"CLSID", clsidText) || !guidParse(clsidText, clsid)) continue;
        bagString(bag.get(), L"FriendlyName", friendly);
        std::string name = narrow(friendly);

        bool skip = false;
        for (const GUID* c : skipFilters) if (clsid == *c) skip = true;
        if (skip || nameIsNotAnEffect(name)) continue;

        // The mapper also matches filters whose pins accept anything (tees, grabbers, pass-through helpers). Where the
        // registration can be read, at least one pin must say "audio".
        std::vector<BYTE> data;
        std::vector<RegPin> pins;
        if (bagBytes(bag.get(), L"FilterData", data) && parseFilterData(data, pins)) {
            bool saysAudio = false;
            for (const auto& p : pins)
                for (const auto& t : p.types) if (t.first == MEDIATYPE_Audio) saysAudio = true;
            if (!saysAudio) continue;
        }

        if (name.empty()) name = guidText(clsid);
        if (!seen.insert(toLower(guidText(clsid))).second) continue;      // a filter can be in several categories
        // property pages: nearly every DirectX plug-in has them, and only loading it can tell
        found.push_back(dxDesc(kDshowPrefix, clsid, name, true));
    }
    std::sort(found.begin(), found.end(), lessByName);
    out.insert(out.end(), found.begin(), found.end());
}

}  // namespace

std::vector<AudioPluginDesc> listDirectXAudioPlugins()
{
    std::vector<AudioPluginDesc> out;
    ScopedCom com;
    if (!com.ok()) return out;
    std::set<std::string> seen;         // class ids already listed
    listDmoEffects(out, seen);
    listDirectShowEffects(out, seen);
    return out;
}

std::unique_ptr<AudioPlugin> dxCreate(const AudioPluginDesc& desc, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::unique_ptr<AudioPlugin>();
    };
    if (!comEnsure(true)) return fail("could not be created (COM is not available on this thread)");

    // "dmo:{...}" or "dshow:{...}"; a bare "{...}" is looked up among the DMOs first
    std::string kind, id = desc.uid;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) { kind = toLower(id.substr(0, colon)); id = id.substr(colon + 1); }
    GUID clsid = GUID_NULL;
    if (!guidParse(widen(id), clsid)) return fail("has no usable class id (" + desc.uid + ")");
    if (kind.empty()) {
        wchar_t name[80] = {0};
        kind = DMOGetName(clsid, name) == S_OK ? "dmo" : "dshow";
    }

    std::string why;
    if (kind == "dmo") {
        std::unique_ptr<DmoPlugin> p(new DmoPlugin());
        p->desc = desc;
        if (!p->init(clsid, &why)) return fail(why);
        return std::unique_ptr<AudioPlugin>(p.release());
    }
    if (kind == "dshow") {
        std::unique_ptr<DsPlugin> p(new DsPlugin());
        p->desc = desc;
        if (!p->init(clsid, &why)) {
            // a project from VEGAS names every DirectX plug-in "dshow:"; one that is really a DMO is run as one
            wchar_t name[80] = {0};
            if (DMOGetName(clsid, name) == S_OK) {
                std::unique_ptr<DmoPlugin> d(new DmoPlugin());
                d->desc = desc;
                std::string why2;
                if (d->init(clsid, &why2)) return std::unique_ptr<AudioPlugin>(d.release());
            }
            return fail(why);
        }
        return std::unique_ptr<AudioPlugin>(p.release());
    }
    return fail("is an unknown kind of DirectX plug-in (" + desc.uid + ")");
}

}  // namespace reno

#endif  // _WIN32
