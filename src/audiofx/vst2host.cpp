// VST2 effect host.
#include "audioplugin.h"
#include "../core/dynlib.h"
#include "vst.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <atomic>
#include <unordered_map>
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

// Opcodes by number (the header in third_party names most but not all).
enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14, effEditClose = 15,
    effEditIdle = 19, effGetChunk = 23, effSetChunk = 24, effGetPlugCategory = 35, effGetEffectName = 45, effGetVendorString = 47,
    effGetProductString = 48, effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58, effStartProcess = 71, effStopProcess = 72,
    effSetProcessPrecision = 77, effSetSpeakerArrangement = 42, effGetSpeakerArrangement = 69
};
enum {
    amAutomate = 0, amVersion = 1, amCurrentId = 2, amIdle = 3, amWantMidi = 6, amGetTime = 7, amProcessEvents = 8, amIOChanged = 13,
    amNeedIdle = 14, amSizeWindow = 15, amGetSampleRate = 16, amGetBlockSize = 17, amGetInputLatency = 18, amGetOutputLatency = 19,
    amGetCurrentProcessLevel = 23, amGetAutomationState = 24, amGetVendorString = 32, amGetProductString = 33, amGetVendorVersion = 34,
    amCanDo = 37, amGetLanguage = 38, amGetDirectory = 41, amUpdateDisplay = 42, amBeginEdit = 43, amEndEdit = 44
};
enum { kFlagHasEditor = 1 << 0, kFlagCanReplacing = 1 << 4, kFlagProgramChunks = 1 << 5, kFlagIsSynth = 1 << 8 };

#pragma pack(push, 8)
struct TimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
#pragma pack(pop)
enum { kTransportPlaying = 1 << 1, kPpqPosValid = 1 << 9, kTempoValid = 1 << 10, kTimeSigValid = 1 << 13 };

class Vst2Plugin;
thread_local Vst2Plugin* tLoading = nullptr;

// Which of our plug-ins an effect is, for the host callback. Looked up instead of read from the effect: a pointer
// stored in the effect's own structure is only as reliable as the plug-in, and some plug-ins (shells, wrappers) call
// back with another effect than the one they handed us. Unknown: no plug-in.
std::mutex gEffectsMutex;
std::unordered_map<const vst_effect_t*, Vst2Plugin*>& effects()
{
    static std::unordered_map<const vst_effect_t*, Vst2Plugin*>* m = new std::unordered_map<const vst_effect_t*, Vst2Plugin*>();
    return *m;
}

// On Windows a plug-in that crashes (an access violation in its own code) is stopped there and not called again, as
// VEGAS does, instead of taking the whole program down; OpenFX effects are guarded the same way (ofxhost).
#ifdef _WIN32
intptr_t guardedControl(vst_effect_t* fx, int32_t op, int32_t index, intptr_t value, void* ptr, float opt, unsigned long* code)
{
    __try {
        return fx->control(fx, op, index, value, ptr, opt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return 0;
    }
}
void guardedProcess(vst_effect_process_float_t fn, vst_effect_t* fx, const float* const* in, float** out, int32_t n, unsigned long* code)
{
    __try {
        fn(fx, in, out, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
    }
}
float guardedGet(vst_effect_t* fx, uint32_t index, unsigned long* code)
{
    __try {
        return fx->get_parameter(fx, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return 0.f;
    }
}
void guardedSet(vst_effect_t* fx, uint32_t index, float v, unsigned long* code)
{
    __try {
        fx->set_parameter(fx, index, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
    }
}
vst_effect_t* guardedMain(vst_effect_t* (VST_FUNCTION_INTERFACE* mainFn)(vst_host_callback_t), vst_host_callback_t cb, unsigned long* code)
{
    __try {
        return mainFn(cb);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return nullptr;
    }
}
#endif

typedef vst_effect_t* (VST_FUNCTION_INTERFACE* MainFn)(vst_host_callback_t);

intptr_t VST_FUNCTION_INTERFACE hostCallback(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t value, const char* ptr, float opt);

class Vst2Plugin : public AudioPlugin {
public:
    DynLib lib;
    vst_effect_t* fx = nullptr;
    double sampleRate = 48000.0;
    int blockSize = 1024;
    bool active = false, editorOpen = false;
    TimeInfo time;
    std::vector<float> inBuf[2], outBuf[2], extra;
    std::vector<const float*> ins;
    std::vector<float*> outs;
    std::mutex mutex;                   // process() against activate()/state changes

    std::atomic<bool> crashed{false};   // (Windows) it crashed once: never called again
    std::string folder;                 // its file's folder (audioMasterGetDirectory)

    ~Vst2Plugin() override
    {
        closeEditor();
        deactivate();
        if (fx) dispatch(effClose);
        {
            std::lock_guard<std::mutex> lk(gEffectsMutex);
            if (fx) effects().erase(fx);
        }
        fx = nullptr;
        lib.leak();                     // unloading a VST that has had a window open is unsafe
    }

    void noteCrash(unsigned long code, const char* where)
    {
        if (!code || crashed.exchange(true)) return;
        logf("VST2 %s crashed in %s (exception 0x%08lX): it is not used again", desc.name.c_str(), where, code);
    }

    intptr_t dispatch(int32_t op, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.f)
    {
        if (!fx || !fx->control || crashed.load()) return 0;
#ifdef _WIN32
        unsigned long code = 0;
        const intptr_t r = guardedControl(fx, op, index, value, ptr, opt, &code);
        if (code) noteCrash(code, strfmt("its dispatcher (opcode %d)", (int)op).c_str());
        return r;
#else
        return fx->control(fx, op, index, value, ptr, opt);
#endif
    }
    void callProcess(vst_effect_process_float_t fn, const float* const* in, float** out, int32_t n)
    {
        if (crashed.load()) return;
#ifdef _WIN32
        unsigned long code = 0;
        guardedProcess(fn, fx, in, out, n, &code);
        if (code) noteCrash(code, "processing");
#else
        fn(fx, in, out, n);
#endif
    }

    bool load(const std::string& path, std::string* error)
    {
        std::string e;
        if (!lib.open(path, &e)) { if (error) *error = "could not be loaded (" + e + ")"; return false; }
        MainFn mainFn = (MainFn)lib.symbol("VSTPluginMain");
        if (!mainFn) mainFn = (MainFn)lib.symbol("main");
        if (!mainFn) mainFn = (MainFn)lib.symbol("MAIN");
        if (!mainFn) { if (error) *error = "is not a VST2 plug-in"; return false; }
        std::memset(&time, 0, sizeof(time));
        time.tempo = 120.0;
        time.timeSigNumerator = 4;
        time.timeSigDenominator = 4;
        time.sampleRate = sampleRate;
        folder = pathDir(path);
        tLoading = this;
#ifdef _WIN32
        unsigned long code = 0;
        fx = guardedMain(mainFn, hostCallback, &code);
        if (code) { tLoading = nullptr; fx = nullptr; if (error) *error = strfmt("crashed while starting (exception 0x%08lX)", code); return false; }
#else
        fx = mainFn(hostCallback);
#endif
        tLoading = nullptr;
        if (!fx || fx->magic_number != (int32_t)VST_MAGICNUMBER) { fx = nullptr; if (error) *error = "did not start (no effect returned)"; return false; }
        {
            std::lock_guard<std::mutex> lk(gEffectsMutex);
            effects()[fx] = this;
        }
        tLoading = this;                // effOpen may call back before it knows its effect
        dispatch(effOpen);
        tLoading = nullptr;
        if (crashed.load()) { if (error) *error = "crashed while starting"; return false; }
        char buf[256];
        auto text = [&](int op) {
            std::memset(buf, 0, sizeof(buf));
            dispatch(op, 0, 0, buf);
            buf[255] = 0;
            return std::string(buf);
        };
        desc.format = "VST2";
        desc.path = path;
        desc.uid = strfmt("%u", (unsigned)fx->unique_id);
        desc.name = text(effGetEffectName);
        if (desc.name.empty()) desc.name = text(effGetProductString);
        if (desc.name.empty()) desc.name = pathStem(path);
        desc.vendor = text(effGetVendorString);
        desc.numInputs = fx->num_inputs;
        desc.numOutputs = fx->num_outputs;
        desc.hasEditor = (fx->flags & kFlagHasEditor) != 0;
        desc.isInstrument = (fx->flags & kFlagIsSynth) != 0;
        return true;
    }

    bool activate(double sr, int maxBlock) override
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!fx) return false;
        if (active) { dispatch(effStopProcess); dispatch(effMainsChanged, 0, 0); active = false; }
        sampleRate = sr;
        blockSize = maxBlock < 16 ? 16 : maxBlock;
        time.sampleRate = sr;
        dispatch(effSetSampleRate, 0, 0, nullptr, (float)sr);
        dispatch(effSetBlockSize, 0, blockSize);
        dispatch(effSetProcessPrecision, 0, 0);
        dispatch(effMainsChanged, 0, 1);
        dispatch(effStartProcess);
        for (int c = 0; c < 2; ++c) { inBuf[c].assign((size_t)blockSize, 0.f); outBuf[c].assign((size_t)blockSize, 0.f); }
        extra.assign((size_t)blockSize, 0.f);
        for (auto& b : six) b.assign(chans == 6 ? (size_t)blockSize : 0, 0.f);
        active = true;
        return true;
    }

    void deactivate() override
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!fx || !active) return;
        dispatch(effStopProcess);
        dispatch(effMainsChanged, 0, 0);
        active = false;
    }

    // ---- six channels (setChannels): VEGAS's way with a 5.1 master. A plug-in with six inputs and outputs (or that
    // gets them when asked for VST's 5.1 speaker arrangement and says so with audioMasterIOChanged) processes
    // L R C LFE Ls Rs as one stream; anything else stays stereo.
    int chans = 2;
    std::vector<float> six[6];

    static void arrangement(vst_speaker_arrangement_t& a, bool surround)
    {
        std::memset(&a, 0, sizeof(a));
        static const char* const names[6] = {"L", "R", "C", "Lfe", "Ls", "Rs"};
        a.type = surround ? VST_SPEAKER_ARRANGEMENT_TYPE_5_1 : VST_SPEAKER_ARRANGEMENT_TYPE_STEREO;
        a.channels = surround ? 6 : 2;
        for (int c = 0; c < a.channels; ++c) {
            std::snprintf(a.speakers[c].name, sizeof(a.speakers[c].name), "%s", names[c]);
            a.speakers[c].type = c + 1;             // kSpeakerL .. kSpeakerRs
            if (c == 3) a.speakers[c].azimuth = a.speakers[c].altitude = 10.f;     // the LFE has no direction
        }
    }

    // (with 'mutex' held) stops and restarts processing around a channel change
    void restartLocked()
    {
        dispatch(effSetSampleRate, 0, 0, nullptr, (float)sampleRate);
        dispatch(effSetBlockSize, 0, blockSize);
        dispatch(effMainsChanged, 0, 1);
        dispatch(effStartProcess);
    }

    bool setChannels(int n) override
    {
        if (n != 2 && n != 6) return false;
        std::lock_guard<std::mutex> lk(mutex);
        if (!fx) return n == 2;
        if (n == chans) return true;
        const bool wasActive = active;
        if (active) {
            dispatch(effStopProcess);
            dispatch(effMainsChanged, 0, 0);
            active = false;
        }
        bool ok = true;
        if (n == 6) {
            vst_speaker_arrangement_t in, out;
            arrangement(in, true);
            arrangement(out, true);
            const intptr_t said = dispatch(effSetSpeakerArrangement, 0, (intptr_t)&in, &out);
            // six in and six out: 5.1 (a plug-in made only for six channels may not answer the request)
            ok = fx->num_inputs >= 6 && fx->num_outputs >= 6 && (said != 0 || (fx->num_inputs == 6 && fx->num_outputs == 6));
            if (!ok) {
                arrangement(in, false);
                arrangement(out, false);
                dispatch(effSetSpeakerArrangement, 0, (intptr_t)&in, &out);
            }
        } else {
            vst_speaker_arrangement_t in, out;
            arrangement(in, false);
            arrangement(out, false);
            dispatch(effSetSpeakerArrangement, 0, (intptr_t)&in, &out);
        }
        chans = ok ? n : 2;
        for (auto& b : six) b.assign(chans == 6 ? (size_t)blockSize : 0, 0.f);
        if (wasActive) {
            restartLocked();
            active = true;
        }
        return ok;
    }
    int channels() const override { return chans; }

    void processChannels(float* const* io, int channels, int frames, int64_t samplePos, bool playing) override
    {
        if (chans != 6 || channels < 6) {
            if (channels >= 2) process(io[0], io[1], frames, samplePos, playing);
            return;
        }
        std::unique_lock<std::mutex> lk(mutex, std::try_to_lock);
        if (!lk.owns_lock() || !fx || !active || chans != 6 || crashed.load()) return;     // crashed: sound passes unchanged
        vst_effect_process_float_t fn = fx->process_float;
        if (!fn || !(fx->flags & kFlagCanReplacing)) return;
        const int nIn = fx->num_inputs, nOut = fx->num_outputs;
        if (nIn < 6 || nOut < 6) return;
        if ((int)spare.size() < blockSize) spare.assign((size_t)blockSize, 0.f);
        if ((int)extra.size() < blockSize) extra.assign((size_t)blockSize, 0.f);
        for (auto& b : six)
            if ((int)b.size() < blockSize) b.assign((size_t)blockSize, 0.f);
        int done = 0;
        while (done < frames) {
            const int n = std::min(blockSize, frames - done);
            time.samplePos = (double)(samplePos + done);
            {
                const ProjectTempo pt = projectTempo();     // the project's tempo and time signature
                time.tempo = pt.bpm;
                time.timeSigNumerator = pt.beatsPerMeasure;
                time.timeSigDenominator = pt.beatUnit;
            }
            time.ppqPos = time.samplePos / sampleRate * (time.tempo / 60.0);
            time.flags = kTempoValid | kTimeSigValid | kPpqPosValid | (playing ? kTransportPlaying : 0);
            ins.resize((size_t)nIn);
            outs.resize((size_t)nOut);
            std::memset(extra.data(), 0, (size_t)n * sizeof(float));
            for (int c = 0; c < 6; ++c) std::memcpy(six[c].data(), io[c] + done, (size_t)n * sizeof(float));
            for (int c = 0; c < nIn; ++c) ins[(size_t)c] = c < 6 ? six[c].data() : extra.data();
            // in and out are separate buffers: a plug-in may write an output before reading every input
            std::vector<float>* outBufs[6] = {&outBuf[0], &outBuf[1], &out6[0], &out6[1], &out6[2], &out6[3]};
            for (int c = 0; c < 6; ++c) {
                if ((int)outBufs[c]->size() < blockSize) outBufs[c]->assign((size_t)blockSize, 0.f);
                std::memset(outBufs[c]->data(), 0, (size_t)n * sizeof(float));
            }
            for (int c = 0; c < nOut; ++c) outs[(size_t)c] = c < 6 ? outBufs[c]->data() : spare.data();
            callProcess(fn, ins.data(), outs.data(), n);
            for (int c = 0; c < 6; ++c) std::memcpy(io[c] + done, outBufs[c]->data(), (size_t)n * sizeof(float));
            done += n;
        }
    }
    std::vector<float> out6[4];

    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override
    {
        std::unique_lock<std::mutex> lk(mutex, std::try_to_lock);
        if (!lk.owns_lock() || !fx || !active || fx->num_outputs < 1 || crashed.load()) return;
        vst_effect_process_float_t fn = fx->process_float;
        if (!fn || !(fx->flags & kFlagCanReplacing)) return;
        const int nIn = fx->num_inputs, nOut = fx->num_outputs;
        int done = 0;
        while (done < frames) {
            const int n = std::min(blockSize, frames - done);
            time.samplePos = (double)(samplePos + done);
            {
                const ProjectTempo pt = projectTempo();     // the project's tempo and time signature
                time.tempo = pt.bpm;
                time.timeSigNumerator = pt.beatsPerMeasure;
                time.timeSigDenominator = pt.beatUnit;
            }
            time.ppqPos = time.samplePos / sampleRate * (time.tempo / 60.0);
            time.flags = kTempoValid | kTimeSigValid | kPpqPosValid | (playing ? kTransportPlaying : 0);
            if (nIn == 1) {
                for (int k = 0; k < n; ++k) inBuf[0][(size_t)k] = 0.5f * (left[done + k] + right[done + k]);
            } else {
                std::memcpy(inBuf[0].data(), left + done, (size_t)n * sizeof(float));
                std::memcpy(inBuf[1].data(), right + done, (size_t)n * sizeof(float));
            }
            std::memset(extra.data(), 0, (size_t)n * sizeof(float));
            ins.resize((size_t)std::max(nIn, 1));
            outs.resize((size_t)nOut);
            for (int c = 0; c < nIn; ++c) ins[(size_t)c] = c < 2 ? inBuf[c].data() : extra.data();
            // outputs beyond the first two go to a spare buffer nobody listens to
            if ((int)spare.size() < blockSize) spare.assign((size_t)blockSize, 0.f);
            for (int c = 0; c < nOut; ++c) outs[(size_t)c] = c < 2 ? outBuf[c].data() : spare.data();
            std::memset(outBuf[0].data(), 0, (size_t)n * sizeof(float));
            std::memset(outBuf[1].data(), 0, (size_t)n * sizeof(float));
            callProcess(fn, ins.data(), outs.data(), n);
            std::memcpy(left + done, outBuf[0].data(), (size_t)n * sizeof(float));
            std::memcpy(right + done, (nOut >= 2 ? outBuf[1] : outBuf[0]).data(), (size_t)n * sizeof(float));
            done += n;
        }
    }

    int latencySamples() override { return fx ? fx->delay : 0; }
    int paramCount() override { return fx ? fx->num_params : 0; }

    std::string paramString(int op, int index)
    {
        char buf[256];
        std::memset(buf, 0, sizeof(buf));
        dispatch(op, index, 0, buf);
        buf[255] = 0;
        return std::string(buf);
    }
    std::string paramName(int index) override { return paramString(effGetParamName, index); }
    std::string paramText(int index) override
    {
        std::string v = paramString(effGetParamDisplay, index), u = paramString(effGetParamLabel, index);
        while (!v.empty() && v.back() == ' ') v.pop_back();
        return u.empty() ? v : v + " " + u;
    }
    float paramGet(int index) override
    {
        if (!fx || !fx->get_parameter || index < 0 || index >= fx->num_params || crashed.load()) return 0.f;
#ifdef _WIN32
        unsigned long code = 0;
        const float v = guardedGet(fx, (uint32_t)index, &code);
        if (code) noteCrash(code, "getParameter");
        return v;
#else
        return fx->get_parameter(fx, (uint32_t)index);
#endif
    }
    void paramSet(int index, float v) override
    {
        if (!fx || !fx->set_parameter || index < 0 || index >= fx->num_params || crashed.load()) return;
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
#ifdef _WIN32
        unsigned long code = 0;
        guardedSet(fx, (uint32_t)index, v, &code);
        if (code) noteCrash(code, "setParameter");
#else
        fx->set_parameter(fx, (uint32_t)index, v);
#endif
    }

    // "Q:" the plug-in's chunk of its current program (what VEGAS stores in a .veg, and what an .fxp holds), "C:" its
    // chunk of the whole bank (for a plug-in with no program chunk), "P:" every parameter's value.
    std::string saveState() override
    {
        if (!fx) return std::string();
        if (fx->flags & kFlagProgramChunks) {
            void* data = nullptr;
            intptr_t n = dispatch(effGetChunk, 1, 0, &data);
            if (n > 0 && data) return "Q:" + base64Encode(data, (size_t)n);
            data = nullptr;
            n = dispatch(effGetChunk, 0, 0, &data);
            if (n > 0 && data) return "C:" + base64Encode(data, (size_t)n);
        }
        std::vector<float> v((size_t)std::max(0, fx->num_params));
        for (int k = 0; k < fx->num_params; ++k) v[(size_t)k] = paramGet(k);
        return "P:" + base64Encode(v.data(), v.size() * sizeof(float));
    }

    bool loadState(const std::string& s) override
    {
        if (!fx || s.size() < 2) return false;
        std::string raw = base64Decode(s.substr(2));
        if ((s[0] == 'C' || s[0] == 'Q') && (fx->flags & kFlagProgramChunks)) {
            // effSetChunk's index: 1 the chunk is one program's, 0 the whole bank's
            std::lock_guard<std::mutex> lk(mutex);
            dispatch(effSetChunk, s[0] == 'Q' ? 1 : 0, (intptr_t)raw.size(), (void*)raw.data());
            return true;
        }
        if (s[0] == 'P') {
            const size_t n = std::min(raw.size() / sizeof(float), (size_t)std::max(0, fx->num_params));
            for (size_t k = 0; k < n; ++k) { float f; std::memcpy(&f, raw.data() + k * sizeof(float), sizeof(float)); paramSet((int)k, f); }
            return true;
        }
        return false;
    }

    // ---- programs
    int programCount() override { return fx ? std::max(0, fx->num_programs) : 0; }
    std::string programName(int index) override
    {
        if (!fx || index < 0 || index >= fx->num_programs) return std::string();
        char buf[256] = {0};
        // effGetProgramNameIndexed (29); plug-ins without it are asked only for the current program's name (5)
        if (!dispatch(29, index, -1, buf) && index == currentProgram()) dispatch(5, 0, 0, buf);
        buf[sizeof(buf) - 1] = 0;
        std::string n = buf;
        while (!n.empty() && n.back() == ' ') n.pop_back();
        return n;
    }
    int currentProgram() override { return fx ? (int)dispatch(effGetProgram) : -1; }
    bool setProgram(int index) override
    {
        if (!fx || index < 0 || index >= fx->num_programs) return false;
        std::lock_guard<std::mutex> lk(mutex);
        dispatch(67);                   // effBeginSetProgram
        dispatch(effSetProgram, 0, index);
        dispatch(68);                   // effEndSetProgram
        return true;
    }

    // ---- .fxp / .fxb (Steinberg's preset files: big-endian, 'CcnK' then 'FxCk' / 'FPCh' for a program, 'FxBk' /
    // 'FBCh' for a bank; the 'Ch' kinds hold the plug-in's own chunk, the others its parameters)
    static void put32(std::string& s, uint32_t v) { for (int k = 3; k >= 0; --k) s.push_back((char)((v >> (8 * k)) & 0xFF)); }
    static void putTag(std::string& s, const char* t) { s.append(t, 4); }
    static uint32_t get32(const std::string& s, size_t at) { uint32_t v = 0; for (int k = 0; k < 4; ++k) v = (v << 8) | (uint8_t)s[at + (size_t)k]; return v; }
    static void putFloat(std::string& s, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(s, v); }
    static float getFloat(const std::string& s, size_t at) { const uint32_t v = get32(s, at); float f; std::memcpy(&f, &v, 4); return f; }

    // the current program as an .fxp (parameters, or the plug-in's program chunk)
    std::string programFile(bool chunky)
    {
        std::string body;
        put32(body, 1);                                     // format version
        put32(body, (uint32_t)fx->unique_id);
        put32(body, (uint32_t)fx->version);
        put32(body, (uint32_t)std::max(0, fx->num_params));
        char name[28] = {0};
        const std::string pn = programName(std::max(0, currentProgram()));
        std::memcpy(name, pn.data(), std::min(pn.size(), sizeof(name) - 1));
        body.append(name, sizeof(name));
        if (chunky) {
            void* data = nullptr;
            const intptr_t n = dispatch(effGetChunk, 1, 0, &data);     // 1: just this program
            if (n <= 0 || !data) return std::string();
            put32(body, (uint32_t)n);
            body.append((const char*)data, (size_t)n);
        } else {
            for (int k = 0; k < fx->num_params; ++k) putFloat(body, paramGet(k));
        }
        std::string out;
        putTag(out, "CcnK");
        put32(out, (uint32_t)(body.size() + 4));
        putTag(out, chunky ? "FPCh" : "FxCk");
        return out + body;
    }

    std::string saveVstPresetFile(bool bank) override
    {
        if (!fx) return std::string();
        const bool chunky = (fx->flags & kFlagProgramChunks) != 0;
        if (!bank) return programFile(chunky);
        std::string body;
        put32(body, 2);                                     // format version (2: the current program is kept)
        put32(body, (uint32_t)fx->unique_id);
        put32(body, (uint32_t)fx->version);
        put32(body, (uint32_t)std::max(0, fx->num_programs));
        std::string future(128, '\0');
        const int cur = std::max(0, currentProgram());
        for (int k = 0; k < 4; ++k) future[(size_t)k] = (char)(((uint32_t)cur >> (8 * (3 - k))) & 0xFF);
        body += future;
        if (chunky) {
            void* data = nullptr;
            const intptr_t n = dispatch(effGetChunk, 0, 0, &data);     // 0: the whole bank
            if (n <= 0 || !data) return std::string();
            put32(body, (uint32_t)n);
            body.append((const char*)data, (size_t)n);
        } else {
            // every program in turn (the current one is selected again afterwards)
            for (int p = 0; p < fx->num_programs; ++p) {
                setProgram(p);
                body += programFile(false);
            }
            setProgram(cur);
        }
        std::string out;
        putTag(out, "CcnK");
        put32(out, (uint32_t)(body.size() + 4));
        putTag(out, chunky ? "FBCh" : "FxBk");
        return out + body;
    }

    // One program's file (at 'at'), put into the current program. 'size' is what is left of the bytes.
    bool loadProgramAt(const std::string& b, size_t at, std::string* error)
    {
        if (b.size() < at + 56 || b.compare(at, 4, "CcnK") != 0) { if (error) *error = "not a VST preset file"; return false; }
        const std::string kind = b.substr(at + 8, 4);
        if ((int32_t)get32(b, at + 16) != fx->unique_id) { if (error) *error = "the preset was saved by another plug-in"; return false; }
        const uint32_t nParams = get32(b, at + 24);
        if (kind == "FxCk") {
            if (b.size() < at + 56 + (size_t)nParams * 4) { if (error) *error = "the preset file is cut short"; return false; }
            for (uint32_t k = 0; k < nParams && (int)k < fx->num_params; ++k) paramSet((int)k, getFloat(b, at + 56 + (size_t)k * 4));
        } else if (kind == "FPCh") {
            const uint32_t n = get32(b, at + 56);
            if (!(fx->flags & kFlagProgramChunks)) { if (error) *error = "the preset holds a chunk this plug-in does not take"; return false; }
            if (b.size() < at + 60 + (size_t)n) { if (error) *error = "the preset file is cut short"; return false; }
            std::string chunk = b.substr(at + 60, n);
            std::lock_guard<std::mutex> lk(mutex);
            dispatch(effSetChunk, 1, (intptr_t)chunk.size(), (void*)chunk.data());
        } else {
            if (error) *error = "not a program (.fxp) file";
            return false;
        }
        char name[29] = {0};
        std::memcpy(name, b.data() + at + 28, 28);
        if (name[0]) dispatch(4, 0, 0, name);           // effSetProgramName
        return true;
    }

    bool loadVstPresetFile(const std::string& b, std::string* error) override
    {
        if (!fx) { if (error) *error = "the plug-in is not loaded"; return false; }
        if (b.size() < 28 || b.compare(0, 4, "CcnK") != 0) { if (error) *error = "not a VST preset file (.fxp or .fxb)"; return false; }
        const std::string kind = b.substr(8, 4);
        if (kind == "FxCk" || kind == "FPCh") return loadProgramAt(b, 0, error);
        if (kind != "FxBk" && kind != "FBCh") { if (error) *error = "not a VST preset file (.fxp or .fxb)"; return false; }
        if ((int32_t)get32(b, 16) != fx->unique_id) { if (error) *error = "the bank was saved by another plug-in"; return false; }
        const uint32_t version = get32(b, 12), nPrograms = get32(b, 24);
        const size_t data = 28 + 128;
        if (b.size() < data) { if (error) *error = "the bank file is cut short"; return false; }
        const int cur = version >= 2 ? (int)get32(b, 28) : 0;
        if (kind == "FBCh") {
            if (!(fx->flags & kFlagProgramChunks)) { if (error) *error = "the bank holds a chunk this plug-in does not take"; return false; }
            const uint32_t n = get32(b, data);
            if (b.size() < data + 4 + (size_t)n) { if (error) *error = "the bank file is cut short"; return false; }
            std::string chunk = b.substr(data + 4, n);
            std::lock_guard<std::mutex> lk(mutex);
            dispatch(effSetChunk, 0, (intptr_t)chunk.size(), (void*)chunk.data());
            return true;
        }
        size_t at = data;
        for (uint32_t p = 0; p < nPrograms && (int)p < fx->num_programs; ++p) {
            if (b.size() < at + 12) break;
            const size_t size = (size_t)get32(b, at + 4) + 8;
            setProgram((int)p);
            if (!loadProgramAt(b, at, error)) return false;
            at += size;
        }
        if (cur >= 0 && cur < fx->num_programs) setProgram(cur);
        return true;
    }

    bool hasEditor() override { return fx && (fx->flags & kFlagHasEditor); }

    bool editorSize(int* w, int* h)
    {
        vst_rect_t* r = nullptr;
        dispatch(effEditGetRect, 0, 0, &r);
        if (!r) return false;
        if (w) *w = r->right - r->left;
        if (h) *h = r->bottom - r->top;
        return (r->right - r->left) > 0 && (r->bottom - r->top) > 0;
    }

    bool openEditor(void* parentWindow, int* w, int* h) override
    {
        if (!hasEditor() || !parentWindow) return false;
        if (editorOpen) closeEditor();
        int ew = 0, eh = 0;
        editorSize(&ew, &eh);                       // some plug-ins only know their size after opening, some must be asked first
        dispatch(effEditOpen, 0, 0, parentWindow);
        editorOpen = true;
        if (editorSize(&ew, &eh)) { if (w) *w = ew; if (h) *h = eh; }
        else { if (w) *w = 400; if (h) *h = 300; }
        return true;
    }

    void closeEditor() override
    {
        if (!editorOpen) return;
        dispatch(effEditClose);
        editorOpen = false;
    }

    void idle() override { if (editorOpen) dispatch(effEditIdle); }

    std::vector<float> spare;
};

intptr_t VST_FUNCTION_INTERFACE hostCallback(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t value, const char* ptr, float opt)
{
    (void)opt;
    Vst2Plugin* self = nullptr;
    if (effect) {
        std::lock_guard<std::mutex> lk(gEffectsMutex);
        auto it = effects().find(effect);
        if (it != effects().end()) self = it->second;
    }
    if (!self) self = tLoading;
    switch (opcode) {
    case amVersion: return 2400;
    case amCurrentId: return 0;
    case amIdle: case amNeedIdle: return 1;
    case amWantMidi: return 0;
    case amGetTime: return self ? (intptr_t)&self->time : 0;
    case amProcessEvents: return 0;
    case amIOChanged: return 1;                // inputs and outputs are counted again every block
    case amSizeWindow:
        if (self && self->onEditorResize) { self->onEditorResize(index, (int)value); return 1; }
        return 0;
    case amGetSampleRate: return self ? (intptr_t)self->sampleRate : 48000;
    case amGetBlockSize: return self ? self->blockSize : 1024;
    case amGetInputLatency: case amGetOutputLatency: return 0;
    case amGetCurrentProcessLevel: return 0;
    case amGetAutomationState: return 1;       // automation off
    case amGetVendorString: if (ptr) std::strcpy((char*)ptr, "Reno"); return 1;
    case amGetProductString: if (ptr) std::strcpy((char*)ptr, "Reno"); return 1;
    case amGetVendorVersion: return 1000;
    case amCanDo:
        if (!ptr) return 0;
        if (!std::strcmp(ptr, "sendVstTimeInfo") || !std::strcmp(ptr, "sizeWindow") || !std::strcmp(ptr, "supplyIdle") ||
            !std::strcmp(ptr, "startStopProcess")) return 1;
        return 0;
    case amGetLanguage: return 1;              // English
    case amGetDirectory: return self ? (intptr_t)self->folder.c_str() : 0;     // the plug-in's folder
    case amUpdateDisplay: return 1;
    case amAutomate:
        if (self && self->onParamChangedByPlugin) self->onParamChangedByPlugin(index);
        return 0;
    case amBeginEdit: case amEndEdit: return 1;
    default: return 0;
    }
}

}  // namespace

std::vector<AudioPluginDesc> vst2Describe(const std::string& path, std::string* error)
{
    std::vector<AudioPluginDesc> out;
    std::unique_ptr<Vst2Plugin> p(new Vst2Plugin());
    if (p->load(path, error)) out.push_back(p->desc);
    return out;
}

std::unique_ptr<AudioPlugin> vst2Create(const AudioPluginDesc& desc, std::string* error)
{
    std::unique_ptr<Vst2Plugin> p(new Vst2Plugin());
    if (!p->load(desc.path, error)) return nullptr;
    return std::unique_ptr<AudioPlugin>(p.release());
}

}  // namespace reno
