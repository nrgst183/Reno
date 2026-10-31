// The Vulkan device, buffers, pipelines and the runner of plans (see gpu.h).
#include "vkgpu.h"
#include "vkapi.h"
#include "vkshaderlib.h"
#include "../core/base.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>

namespace reno {
namespace gpu {

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

const int kBindings = 4;
const uint32_t kMaxPasses = 4096;                // passes (and so descriptor sets) in one frame's plan: Radial Blur runs hundreds of small steps                    // in0, in1, out, table
const uint32_t kMaxPush = 128;
const size_t kPoolKeepBytes = (size_t)768 << 20;
const size_t kAlign = 256;

size_t alignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// ---------------------------------------------------------------------------------------------------- a buffer
enum class Kind { Device = 0, StageUp = 1, StageDown = 2 };

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    size_t size = 0;
    void* mapped = nullptr;
    Kind kind = Kind::Device;
    bool coherent = true;
};

struct Resident {
    Buffer buf;
    uint64_t serial = 0;
    uint64_t epoch = 0;
    uint32_t w = 0, h = 0;
    bool isFloat = false, bgra = true;
    uint64_t hash = 0;
};

// A hash of some forty-odd rows of the frame, sixty-odd pixels each (and its last pixel): cheap, and notices a frame changed in place by something that did not know about the card's copy.
uint64_t sampleHash(const uint8_t* base, size_t rowBytes, uint32_t w, uint32_t h, size_t bpp)
{
    uint64_t hv = 1469598103934665603ull;
    const uint32_t ys = std::max(1u, h / 41), xs = std::max(1u, w / 61);
    for (uint32_t y = 0; y < h; y += ys) {
        const uint8_t* row = base + rowBytes * y;
        for (uint32_t x = 0; x < w; x += xs)
            for (size_t k = 0; k < bpp; ++k) hv = (hv ^ row[(size_t)x * bpp + k]) * 1099511628211ull;
    }
    const uint8_t* last = base + rowBytes * (h - 1) + (size_t)(w - 1) * bpp;
    for (size_t k = 0; k < bpp; ++k) hv = (hv ^ last[k]) * 1099511628211ull;
    return hv;
}

// A frame the last effect left on the card (FrameIO::deferOut), waiting for the next effect of the chain or for materialize().
struct PendingFrame {
    Buffer buf;
    uint32_t w = 0, h = 0;
    bool isFloat = false, bgra = true;
};
const size_t kMaxPending = 8;

// What a pass binds: the buffers (and the part of the table buffer) of its descriptor set. The same sizes come from the buffer pool frame after frame, so the
// same sets are asked for again and again; a thread keeps the sets it made (until a buffer is destroyed).
struct DescKey {
    VkBuffer buf[kBindings];
    VkDeviceSize off[kBindings];
    VkDeviceSize range[kBindings];
    bool operator==(const DescKey& o) const
    {
        for (int k = 0; k < kBindings; ++k)
            if (buf[k] != o.buf[k] || off[k] != o.off[k] || range[k] != o.range[k]) return false;
        return true;
    }
};
struct DescHash {
    size_t operator()(const DescKey& k) const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; h ^= h >> 29; };
        for (int j = 0; j < kBindings; ++j) { mix((uint64_t)(uintptr_t)k.buf[j]); mix((uint64_t)k.off[j]); mix((uint64_t)k.range[j]); }
        return (size_t)h;
    }
};
struct PipeInfo { VkPipeline pipe = VK_NULL_HANDLE; uint32_t lx = 1, ly = 1; };

struct ThreadState {
    std::unordered_map<DescKey, VkDescriptorSet, DescHash> sets;
    std::unordered_map<std::string, PipeInfo> pipes;       // (pipelines live as long as the device)
    uint64_t setsGen = 0;                                   // Core::bufGen when the sets were made
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    Resident resident;
    bool busy = false;                      // a thread has it
};

struct Core {
    VkApi api;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    DeviceInfo info;
    VkPhysicalDeviceMemoryProperties mem;
    VkPhysicalDeviceLimits limits;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::string cachePath;
    bool cacheDirty = false;
    float timestampPeriod = 1.f;
    bool timestamps = false;
    Buffer dummy;

    std::mutex mu;                          // pools, pipelines, thread states
    std::mutex queueMu;                     // vkQueueSubmit
    std::map<std::string, VkPipeline> pipelines;
    std::map<std::string, std::pair<uint32_t, uint32_t>> localSizes;
    std::multimap<std::pair<int, size_t>, Buffer> freeBuffers;
    size_t freeBytes = 0;
    std::vector<std::unique_ptr<ThreadState>> states;
    std::map<uint64_t, PendingFrame> pending;      // (under mu)
    std::atomic<uint64_t> bufGen{1};        // counts destroyed buffers: descriptor sets naming a destroyed buffer must not be used again
    std::atomic<bool> lost{false};
    std::atomic<int> oomCount{0};
    uint64_t id = 0;
    std::atomic<uint64_t> residentEpoch{1};
};

struct Global {
    std::mutex mu;
    bool enabled = false;
    int selected = -1;
    bool tried = false;
    std::string failure;
    std::shared_ptr<Core> core;
    std::atomic<uint64_t> epoch{1};
};
Global& G()
{
    static Global* g = new Global();
    return *g;
}

// ---------------------------------------------------------------------------------------------------- devices
std::string versionText(uint32_t v)
{
    return strfmt("%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

int scoreOf(const DeviceInfo& d)
{
    switch (d.type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return 1;
    default: return 0;
    }
}

bool makeInstance(VkApi& api, VkInstance& inst, std::string* why)
{
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Reno";
    app.applicationVersion = 1;
    app.pEngineName = "Reno";
    app.engineVersion = 1;
    // Vulkan 1.2 (float controls are core in it); a loader or driver that does not take that gets 1.0, as before
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 2, 0);
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    // macOS: Vulkan there is MoltenVK, a "portability" implementation the loader lists only when asked to
    const char* portability = "VK_KHR_portability_enumeration";
    if (api.vkEnumerateInstanceExtensionProperties) {
        uint32_t n = 0;
        api.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> ext(n);
        if (n) api.vkEnumerateInstanceExtensionProperties(nullptr, &n, ext.data());
        for (const VkExtensionProperties& e : ext)
            if (std::strcmp(e.extensionName, portability) == 0) {
                ci.enabledExtensionCount = 1;
                ci.ppEnabledExtensionNames = &portability;
                ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
    }
    inst = VK_NULL_HANDLE;
    api.instanceApiVersion = app.apiVersion;
    VkResult r = api.vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS || !inst) {
        app.apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        api.instanceApiVersion = app.apiVersion;
        inst = VK_NULL_HANDLE;
        r = api.vkCreateInstance(&ci, nullptr, &inst);
    }
    if (r != VK_SUCCESS || !inst) {
        if (why) *why = strfmt("vkCreateInstance failed (%d)", (int)r);
        inst = VK_NULL_HANDLE;
        return false;
    }
    if (!loadInstanceFunctions(api, inst, why)) {
        if (api.vkDestroyInstance) api.vkDestroyInstance(inst, nullptr);
        inst = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// The physical devices with what Reno needs to know of each.
void describeDevices(VkApi& api, VkInstance inst, std::vector<VkPhysicalDevice>& phys, std::vector<DeviceInfo>& out)
{
    uint32_t n = 0;
    phys.clear();
    out.clear();
    if (api.vkEnumeratePhysicalDevices(inst, &n, nullptr) != VK_SUCCESS || n == 0) return;
    phys.resize(n);
    if (api.vkEnumeratePhysicalDevices(inst, &n, phys.data()) < 0) { phys.clear(); return; }
    phys.resize(n);
    for (uint32_t k = 0; k < n; ++k) {
        DeviceInfo d;
        d.index = (int)k;
        VkPhysicalDeviceProperties pr = {};
        api.vkGetPhysicalDeviceProperties(phys[k], &pr);
        d.name = pr.deviceName;
        d.vendorId = pr.vendorID;
        d.deviceId = pr.deviceID;
        d.apiVersion = pr.apiVersion;
        d.driverVersion = pr.driverVersion;
        d.type = (int)pr.deviceType;
        d.software = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        {
            std::string low = toLower(d.name);
            if (low.find("swiftshader") != std::string::npos || low.find("llvmpipe") != std::string::npos || low.find("lavapipe") != std::string::npos ||
                low.find("software") != std::string::npos || low.find("warp") != std::string::npos || low.find("basic render") != std::string::npos)
                d.software = true;
        }
        // the float controls: how 32-bit floating point behaves in the kernels (asked with Vulkan 1.2's properties)
        if (api.vkGetPhysicalDeviceProperties2 && api.instanceApiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0) && pr.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
            VkPhysicalDeviceFloatControlsProperties fc = {};
            fc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
            VkPhysicalDeviceProperties2 p2 = {};
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &fc;
            api.vkGetPhysicalDeviceProperties2(phys[k], &p2);
            d.fcKnown = true;
            d.fcRteF32 = fc.shaderRoundingModeRTEFloat32 != 0;
            d.fcRtzF32 = fc.shaderRoundingModeRTZFloat32 != 0;
            d.fcDenormPreserveF32 = fc.shaderDenormPreserveFloat32 != 0;
            d.fcDenormFlushF32 = fc.shaderDenormFlushToZeroFloat32 != 0;
            d.fcSignedZeroInfNanF32 = fc.shaderSignedZeroInfNanPreserveFloat32 != 0;
            d.fcDenormIndependence = (int)fc.denormBehaviorIndependence;
            d.fcRoundingIndependence = (int)fc.roundingModeIndependence;
            const char* off = std::getenv("RENO_VK_NO_FLOAT_CONTROLS");
            const char* force = std::getenv("RENO_VK_FORCE_FLOAT_CONTROLS");        // (for trying the fallback on a driver that does not have them)
            // each control the driver has is used (RTE rounding, keeping denormals); forcing asks for both
            const bool forced = force && force[0] && force[0] != '0';
            d.fcUseRte = d.fcRteF32 || forced;
            d.fcUseDenorm = d.fcDenormPreserveF32 || forced;
            d.fcUseSzinf = d.fcSignedZeroInfNanF32 || forced;       // (the kernels test for NaN and infinity and rely on the sign of zero)
            d.floatControlsUsed = (d.fcUseRte || d.fcUseDenorm || d.fcUseSzinf) && !(off && off[0] && off[0] != '0');
        }
        d.maxStorageRange = pr.limits.maxStorageBufferRange;
        d.maxPushConstants = pr.limits.maxPushConstantsSize;
        d.maxGroupInvocations = pr.limits.maxComputeWorkGroupInvocations;
        VkPhysicalDeviceMemoryProperties mp = {};
        api.vkGetPhysicalDeviceMemoryProperties(phys[k], &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > d.localMemoryBytes) d.localMemoryBytes = mp.memoryHeaps[h].size;
        uint32_t qn = 0;
        api.vkGetPhysicalDeviceQueueFamilyProperties(phys[k], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        if (qn) api.vkGetPhysicalDeviceQueueFamilyProperties(phys[k], &qn, qf.data());
        for (uint32_t q = 0; q < qn; ++q) {
            if ((qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && qf[q].queueCount > 0) {
                d.computeQueueFamily = q;
                d.timestamps = qf[q].timestampValidBits >= 32 && pr.limits.timestampPeriod > 0 && pr.limits.timestampComputeAndGraphics;
                break;
            }
        }
        if (d.computeQueueFamily == 0xffffffffu) d.note = "no compute queue";
        else if (d.maxPushConstants < 64) d.note = "push constants too small";
        else if (d.maxGroupInvocations < 256) d.note = "workgroups too small";
        else if (d.maxStorageRange < (64u << 20)) d.note = "storage buffers too small";
        else d.usable = true;
        out.push_back(d);
    }
}

// The device to use: the chosen one, else the best that is not a CPU implementation (those only when asked for).
int chooseDevice(const std::vector<DeviceInfo>& devs, int selected)
{
    if (selected >= 0 && selected < (int)devs.size()) return devs[(size_t)selected].usable ? selected : -1;
    if (const char* env = std::getenv("RENO_VK_DEVICE")) {
        const std::string e = env;
        if (!e.empty()) {
            char* end = nullptr;
            const long v = std::strtol(e.c_str(), &end, 10);
            if (end && *end == 0 && v >= 0 && v < (long)devs.size()) return devs[(size_t)v].usable ? (int)v : -1;
            const std::string low = toLower(e);
            for (const auto& d : devs)
                if (d.usable && toLower(d.name).find(low) != std::string::npos) return d.index;
            return -1;
        }
    }
    const char* soft = std::getenv("RENO_VK_ALLOW_SOFTWARE");
    const bool allowSoft = soft && soft[0] && soft[0] != '0';
    int best = -1;
    for (const auto& d : devs) {
        if (!d.usable || (d.software && !allowSoft)) continue;
        if (best < 0) { best = d.index; continue; }
        const DeviceInfo& b = devs[(size_t)best];
        if (scoreOf(d) > scoreOf(b) || (scoreOf(d) == scoreOf(b) && d.localMemoryBytes > b.localMemoryBytes)) best = d.index;
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------- memory
bool findMemoryType(const Core& c, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, uint32_t& index, VkMemoryPropertyFlags& got)
{
    for (uint32_t k = 0; k < c.mem.memoryTypeCount; ++k) {
        if (!(bits & (1u << k))) continue;
        const VkMemoryPropertyFlags f = c.mem.memoryTypes[k].propertyFlags;
        if ((f & want) == want && !(f & avoid)) { index = k; got = f; return true; }
    }
    return false;
}

void destroyBuffer(Core& c, Buffer& b)
{
    if (b.buf) c.bufGen++;
    if (b.mapped) c.api.vkUnmapMemory(c.dev, b.mem);
    if (b.buf) c.api.vkDestroyBuffer(c.dev, b.buf, nullptr);
    if (b.mem) c.api.vkFreeMemory(c.dev, b.mem, nullptr);
    b = Buffer();
}

bool createBuffer(Core& c, size_t size, Kind kind, Buffer& out)
{
    out = Buffer();
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bi.usage = kind == Kind::Device ? (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
                                    : kind == Kind::StageUp ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkResult r = c.api.vkCreateBuffer(c.dev, &bi, nullptr, &out.buf);
    if (r != VK_SUCCESS) { if (r == VK_ERROR_DEVICE_LOST) c.lost = true; out = Buffer(); return false; }
    VkMemoryRequirements rq = {};
    c.api.vkGetBufferMemoryRequirements(c.dev, out.buf, &rq);
    uint32_t type = 0;
    VkMemoryPropertyFlags got = 0;
    bool ok = false;
    if (kind == Kind::Device) {
        ok = findMemoryType(c, rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT, type, got);
    } else if (kind == Kind::StageUp) {
        ok = findMemoryType(c, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT, type, got)
             || findMemoryType(c, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT, type, got);
    } else {
        // reading back: cached memory is much faster for the CPU to read
        ok = findMemoryType(c, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT, type, got)
             || findMemoryType(c, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT, type, got)
             || findMemoryType(c, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT, type, got);
    }
    if (!ok) { destroyBuffer(c, out); return false; }
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = type;
    r = c.api.vkAllocateMemory(c.dev, &ai, nullptr, &out.mem);
    if (r != VK_SUCCESS) {
        if (r == VK_ERROR_DEVICE_LOST) c.lost = true;
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) c.oomCount++;
        destroyBuffer(c, out);
        return false;
    }
    if (c.api.vkBindBufferMemory(c.dev, out.buf, out.mem, 0) != VK_SUCCESS) { destroyBuffer(c, out); return false; }
    out.size = size;
    out.kind = kind;
    out.coherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (kind != Kind::Device) {
        if (c.api.vkMapMemory(c.dev, out.mem, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) { out.mapped = nullptr; destroyBuffer(c, out); return false; }
    }
    return true;
}

size_t roundSize(size_t s)
{
    s = std::max<size_t>(s, 256);
    size_t g = 4096;
    while (g * 64 < s) g *= 2;
    return alignUp(s, g);
}

// A buffer of at least 'size' bytes: one kept from before, or a new one (false when the card has no more memory).
bool takeBuffer(Core& c, size_t size, Kind kind, Buffer& out)
{
    const size_t cap = roundSize(size);
    {
        std::lock_guard<std::mutex> lk(c.mu);
        auto it = c.freeBuffers.find(std::make_pair((int)kind, cap));
        if (it != c.freeBuffers.end()) {
            out = it->second;
            c.freeBytes -= out.size;
            c.freeBuffers.erase(it);
            return true;
        }
        if (c.freeBytes > kPoolKeepBytes / 2) {
            for (auto& kv : c.freeBuffers) destroyBuffer(c, kv.second);
            c.freeBuffers.clear();
            c.freeBytes = 0;
        }
    }
    if (createBuffer(c, cap, kind, out)) return true;
    {
        std::lock_guard<std::mutex> lk(c.mu);
        for (auto& kv : c.freeBuffers) destroyBuffer(c, kv.second);
        c.freeBuffers.clear();
        c.freeBytes = 0;
    }
    return !c.lost && createBuffer(c, cap, kind, out);
}

void giveBuffer(Core& c, Buffer& b)
{
    if (!b.buf) return;
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.lost || c.freeBytes + b.size > kPoolKeepBytes) {
        destroyBuffer(c, b);
        return;
    }
    c.freeBytes += b.size;
    c.freeBuffers.insert(std::make_pair(std::make_pair((int)b.kind, b.size), b));
    b = Buffer();
}

// ---------------------------------------------------------------------------------------------------- pipelines
// The kernel with the 32-bit float controls the driver has added: capabilities DenormPreserve and/or RoundingModeRTE (with the SPV_KHR_float_controls extension, which a SPIR-V 1.0 module needs)
// and, for every entry point, the execution modes DenormPreserve 32 and RoundingModeRTE 32.
std::vector<uint32_t> withFloatControls(const uint32_t* w, size_t n, bool rte, bool denorm, bool szinf)
{
    std::vector<uint32_t> out;
    if (n < 5) return out;
    size_t capEnd = 5, modeEnd = 0;
    std::vector<uint32_t> entries;
    bool testsNan = false;
    for (size_t i = 5; i < n;) {
        const uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (len == 0 || i + len > n) return std::vector<uint32_t>();
        if (op == 17 && capEnd == i) capEnd = i + len;
        if (op == 15) { entries.push_back(w[i + 2]); modeEnd = i + len; }
        if (op == 16) modeEnd = i + len;
        // OpIsNan, OpIsInf, OpOrdered, OpUnordered, the unordered float comparisons
        if (op == 156 || op == 157 || op == 162 || op == 163 || (op >= 181 && op <= 191 && (op & 1))) testsNan = true;
        i += len;
    }
    // SignedZeroInfNanPreserve only for the kernels that test for NaN or infinity (tools/spvlint.py's rule): the rest do not
    // need it, and some drivers compile far slower with it (SwiftShader needs gigabytes for the Iris transition's kernels)
    szinf = szinf && testsNan;
    if (!rte && !denorm && !szinf) return std::vector<uint32_t>();
    if (entries.empty() || modeEnd == 0) return std::vector<uint32_t>();
    out.reserve(n + 16 + entries.size() * 8);
    out.insert(out.end(), w, w + capEnd);
    if (denorm) { out.push_back((2u << 16) | 17u); out.push_back(4464u); }      // DenormPreserve
    if (rte) { out.push_back((2u << 16) | 17u); out.push_back(4467u); }         // RoundingModeRTE
    if (szinf) { out.push_back((2u << 16) | 17u); out.push_back(4466u); }       // SignedZeroInfNanPreserve
    const char* ext = "SPV_KHR_float_controls";
    std::vector<uint32_t> str((std::strlen(ext) + 1 + 3) / 4, 0u);
    std::memcpy(str.data(), ext, std::strlen(ext));
    out.push_back(((uint32_t)(1 + str.size()) << 16) | 10u);
    out.insert(out.end(), str.begin(), str.end());
    out.insert(out.end(), w + capEnd, w + modeEnd);
    for (uint32_t id : entries) {
        if (denorm) { out.push_back((4u << 16) | 16u); out.push_back(id); out.push_back(4459u); out.push_back(32u); }     // DenormPreserve 32
        if (rte) { out.push_back((4u << 16) | 16u); out.push_back(id); out.push_back(4462u); out.push_back(32u); }        // RoundingModeRTE 32
        if (szinf) { out.push_back((4u << 16) | 16u); out.push_back(id); out.push_back(4461u); out.push_back(32u); }      // SignedZeroInfNanPreserve 32
    }
    out.insert(out.end(), w + modeEnd, w + n);
    return out;
}

VkPipeline pipelineFor(Core& c, const std::string& name, uint32_t& lx, uint32_t& ly)
{
    std::lock_guard<std::mutex> lk(c.mu);
    auto it = c.pipelines.find(name);
    if (it != c.pipelines.end()) {
        auto ls = c.localSizes[name];
        lx = ls.first;
        ly = ls.second;
        return it->second;
    }
    const ShaderBlob* blob = findShaderBlob(name);
    if (!blob) return VK_NULL_HANDLE;
    uint32_t x = 0, y = 0, z = 0;
    if (!shaderLocalSize(*blob, x, y, z) || x * y * z > c.info.maxGroupInvocations || x == 0 || y == 0) return VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    bool triedFc = false;
    for (int attempt = 0; attempt < 2 && !pipe; ++attempt) {
        std::vector<uint32_t> fcWords;
        if (attempt == 0 && c.info.floatControlsUsed) { fcWords = withFloatControls(blob->words, blob->count, c.info.fcUseRte, c.info.fcUseDenorm, c.info.fcUseSzinf); triedFc = !fcWords.empty(); }
        if (attempt == 1 && !triedFc) break;
        VkShaderModuleCreateInfo mi = {};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = (fcWords.empty() ? blob->count : fcWords.size()) * 4;
        mi.pCode = fcWords.empty() ? blob->words : fcWords.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (c.api.vkCreateShaderModule(c.dev, &mi, nullptr, &mod) != VK_SUCCESS) {
            if (!fcWords.empty()) { c.info.floatControlsUsed = false; }      // (the driver does not take them: no more tries with them)
            continue;
        }
        VkComputePipelineCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = mod;
        pi.stage.pName = "main";
        pi.layout = c.layout;
        r = c.api.vkCreateComputePipelines(c.dev, c.cache, 1, &pi, nullptr, &pipe);
        c.api.vkDestroyShaderModule(c.dev, mod, nullptr);
        if (r != VK_SUCCESS || !pipe) {
            pipe = VK_NULL_HANDLE;
            if (r == VK_ERROR_DEVICE_LOST) { c.lost = true; return VK_NULL_HANDLE; }
            if (!fcWords.empty()) c.info.floatControlsUsed = false;
        }
    }
    if (!pipe) return VK_NULL_HANDLE;
    c.pipelines[name] = pipe;
    c.localSizes[name] = std::make_pair(x, y);
    c.cacheDirty = true;
    lx = x;
    ly = y;
    return pipe;
}

std::string cacheFilePath(const DeviceInfo& d)
{
    std::string dir = userDataDir();
    if (dir.empty()) return std::string();
    return pathJoin(dir, strfmt("vk_pipelines_%04x_%04x.bin", d.vendorId, d.deviceId));
}

void saveCache(Core& c)
{
    if (!c.cache || c.cachePath.empty() || !c.cacheDirty) return;
    size_t n = 0;
    if (c.api.vkGetPipelineCacheData(c.dev, c.cache, &n, nullptr) != VK_SUCCESS || n == 0 || n > ((size_t)64 << 20)) return;
    std::string data(n, '\0');
    if (c.api.vkGetPipelineCacheData(c.dev, c.cache, &n, &data[0]) < 0) return;
    data.resize(n);
    writeFile(c.cachePath, data);
    c.cacheDirty = false;
}

// ---------------------------------------------------------------------------------------------------- open / close
void destroyCore(Core& c)
{
    if (c.dev) {
        if (c.api.vkDeviceWaitIdle && !c.lost) c.api.vkDeviceWaitIdle(c.dev);
        {
            std::lock_guard<std::mutex> lk(c.mu);
            saveCache(c);
        }
        for (auto& st : c.states) {
            destroyBuffer(c, st->resident.buf);
            if (st->queries) c.api.vkDestroyQueryPool(c.dev, st->queries, nullptr);
            if (st->dpool) c.api.vkDestroyDescriptorPool(c.dev, st->dpool, nullptr);
            if (st->fence) c.api.vkDestroyFence(c.dev, st->fence, nullptr);
            if (st->pool) c.api.vkDestroyCommandPool(c.dev, st->pool, nullptr);
        }
        c.states.clear();
        for (auto& kv : c.pending) destroyBuffer(c, kv.second.buf);
        c.pending.clear();
        for (auto& kv : c.freeBuffers) destroyBuffer(c, kv.second);
        c.freeBuffers.clear();
        destroyBuffer(c, c.dummy);
        for (auto& kv : c.pipelines) c.api.vkDestroyPipeline(c.dev, kv.second, nullptr);
        c.pipelines.clear();
        if (c.cache) c.api.vkDestroyPipelineCache(c.dev, c.cache, nullptr);
        if (c.layout) c.api.vkDestroyPipelineLayout(c.dev, c.layout, nullptr);
        if (c.dsl) c.api.vkDestroyDescriptorSetLayout(c.dev, c.dsl, nullptr);
        c.api.vkDestroyDevice(c.dev, nullptr);
        c.dev = VK_NULL_HANDLE;
    }
    if (c.inst && c.api.vkDestroyInstance) c.api.vkDestroyInstance(c.inst, nullptr);
    c.inst = VK_NULL_HANDLE;
}

std::shared_ptr<Core> openCore(int selected, std::string& why)
{
    std::shared_ptr<Core> c(new Core());
    if (!loadVulkanLoader(c->api, &why)) return nullptr;
    if (!makeInstance(c->api, c->inst, &why)) return nullptr;
    auto fail = [&](const std::string& m) -> std::shared_ptr<Core> {
        why = m;
        destroyCore(*c);
        return nullptr;
    };
    std::vector<VkPhysicalDevice> phys;
    std::vector<DeviceInfo> devs;
    describeDevices(c->api, c->inst, phys, devs);
    if (devs.empty()) return fail("no Vulkan device (no graphics driver with Vulkan support)");
    const int pick = chooseDevice(devs, selected);
    if (pick < 0) {
        std::string m = "no usable Vulkan device";
        bool onlySoft = false;
        for (const auto& d : devs) if (d.usable && d.software) onlySoft = true;
        if (onlySoft) m += " (only a CPU implementation, which is not used unless RENO_VK_ALLOW_SOFTWARE=1 or it is chosen)";
        else for (const auto& d : devs) if (!d.note.empty()) m += strfmt("; %s: %s", d.name.c_str(), d.note.c_str());
        return fail(m);
    }
    c->phys = phys[(size_t)pick];
    c->info = devs[(size_t)pick];
    c->family = c->info.computeQueueFamily;
    c->api.vkGetPhysicalDeviceMemoryProperties(c->phys, &c->mem);
    VkPhysicalDeviceProperties pr = {};
    c->api.vkGetPhysicalDeviceProperties(c->phys, &pr);
    c->limits = pr.limits;
    c->timestampPeriod = pr.limits.timestampPeriod;
    c->timestamps = c->info.timestamps;

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = c->family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di = {};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    // a portability implementation (MoltenVK) must have its subset extension turned on when it offers it
    const char* subset = "VK_KHR_portability_subset";
    {
        uint32_t n = 0;
        c->api.vkEnumerateDeviceExtensionProperties(c->phys, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> ext(n);
        if (n) c->api.vkEnumerateDeviceExtensionProperties(c->phys, nullptr, &n, ext.data());
        for (const VkExtensionProperties& e : ext)
            if (std::strcmp(e.extensionName, subset) == 0) {
                di.enabledExtensionCount = 1;
                di.ppEnabledExtensionNames = &subset;
            }
    }
    VkResult r = c->api.vkCreateDevice(c->phys, &di, nullptr, &c->dev);
    if (r != VK_SUCCESS || !c->dev) { c->dev = VK_NULL_HANDLE; return fail(strfmt("vkCreateDevice failed (%d)", (int)r)); }
    std::string e2;
    if (!loadDeviceFunctions(c->api, c->inst, c->dev, &e2)) return fail(e2);
    c->api.vkGetDeviceQueue(c->dev, c->family, 0, &c->queue);
    if (!c->queue) return fail("no compute queue");

    VkDescriptorSetLayoutBinding bind[kBindings] = {};
    for (int k = 0; k < kBindings; ++k) {
        bind[k].binding = (uint32_t)k;
        bind[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bind[k].descriptorCount = 1;
        bind[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = {};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = kBindings;
    li.pBindings = bind;
    if (c->api.vkCreateDescriptorSetLayout(c->dev, &li, nullptr, &c->dsl) != VK_SUCCESS) return fail("vkCreateDescriptorSetLayout failed");
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, std::min<uint32_t>(kMaxPush, c->info.maxPushConstants)};
    VkPipelineLayoutCreateInfo pli = {};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &c->dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (c->api.vkCreatePipelineLayout(c->dev, &pli, nullptr, &c->layout) != VK_SUCCESS) return fail("vkCreatePipelineLayout failed");

    {
        c->cachePath = cacheFilePath(c->info);
        std::string data;
        if (!c->cachePath.empty()) readFile(c->cachePath, data);
        VkPipelineCacheCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (c->api.vkCreatePipelineCache(c->dev, &ci, nullptr, &c->cache) != VK_SUCCESS) {
            c->cache = VK_NULL_HANDLE;
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            if (c->api.vkCreatePipelineCache(c->dev, &ci, nullptr, &c->cache) != VK_SUCCESS) c->cache = VK_NULL_HANDLE;
        }
    }
    if (!createBuffer(*c, 256, Kind::Device, c->dummy)) return fail("cannot allocate device memory");
    return c;
}

// The core to work with, made on first use. Null (and 'why' set) when there is no usable device.
std::shared_ptr<Core> acquireCore(std::string* why)
{
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.enabled) { if (why) *why = "GPU use is off"; return nullptr; }
    if (g.core && g.core->lost) {
        // a lost device: nothing more is tried with it (switching the GPU setting off and on again opens it anew)
        if (why) *why = "the Vulkan device was lost";
        return nullptr;
    }
    if (!g.core) {
        if (g.tried) { if (why) *why = g.failure; return nullptr; }
        g.tried = true;
        std::string w;
        g.core = openCore(g.selected, w);
        if (!g.core) {
            g.failure = w;
            if (why) *why = w;
            logf("Vulkan: not used (%s); the built-in effects render on the CPU", w.c_str());
            return nullptr;
        }
        logf("Vulkan: using %s for the built-in effects", g.core->info.name.c_str());
        g.failure.clear();
        g.core->id = ++g.epoch;
    }
    return g.core;
}

ThreadState* stateFor(Core& c)
{
    // each thread keeps the state it first took (command pool, fence, ...); a state whose thread is gone is not reused (a thread_local pointer says whose)
    thread_local ThreadState* mine = nullptr;
    thread_local uint64_t mineCore = 0;
    if (mine && mineCore == c.id) return mine;
    std::unique_ptr<ThreadState> st(new ThreadState());
    VkCommandPoolCreateInfo pi = {};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = c.family;
    if (c.api.vkCreateCommandPool(c.dev, &pi, nullptr, &st->pool) != VK_SUCCESS) return nullptr;
    VkCommandBufferAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = st->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkFenceCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxPasses * kBindings};
    VkDescriptorPoolCreateInfo dpi = {};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = kMaxPasses;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    bool ok = c.api.vkAllocateCommandBuffers(c.dev, &ai, &st->cb) == VK_SUCCESS && c.api.vkCreateFence(c.dev, &fi, nullptr, &st->fence) == VK_SUCCESS
              && c.api.vkCreateDescriptorPool(c.dev, &dpi, nullptr, &st->dpool) == VK_SUCCESS;
    if (ok && c.timestamps) {
        VkQueryPoolCreateInfo qi = {};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4;
        if (c.api.vkCreateQueryPool(c.dev, &qi, nullptr, &st->queries) != VK_SUCCESS) st->queries = VK_NULL_HANDLE;
    }
    ThreadState* raw = st.get();
    if (!ok) {
        if (st->fence) c.api.vkDestroyFence(c.dev, st->fence, nullptr);
        if (st->dpool) c.api.vkDestroyDescriptorPool(c.dev, st->dpool, nullptr);
        c.api.vkDestroyCommandPool(c.dev, st->pool, nullptr);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(c.mu);
        c.states.push_back(std::move(st));
    }
    mine = raw;
    mineCore = c.id;
    return raw;
}

// ---------------------------------------------------------------------------------------------------- recording
struct Bound {
    std::string shader;
    VkBuffer buf[kBindings];
    VkDeviceSize off[kBindings];
    VkDeviceSize range[kBindings];
    uint8_t push[kMaxPush];
    uint32_t pushSize = 0;
    uint32_t tx = 0, ty = 1;
};

struct UnpackPush { uint32_t w, h, bgra, pad; };

}  // namespace

// ---------------------------------------------------------------------------------------------------- public: settings
std::string DeviceInfo::typeName() const
{
    if (software) return "CPU (software)";
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU (software)";
    default: return "other";
    }
}
std::string DeviceInfo::apiVersionText() const { return versionText(apiVersion); }

void setEnabled(bool on)
{
    Global& g = G();
    std::shared_ptr<Core> old;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.enabled == on) return;
        g.enabled = on;
        if (!on) old = std::move(g.core);
        g.tried = false;
        g.failure.clear();
    }
    if (old) destroyCore(*old);
}

bool enabled()
{
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.enabled;
}

void selectDevice(int index)
{
    Global& g = G();
    std::shared_ptr<Core> old;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.selected == index) return;
        g.selected = index;
        old = std::move(g.core);
        g.tried = false;
    }
    if (old) destroyCore(*old);
}

std::vector<DeviceInfo> listDevices(std::string* why)
{
    std::vector<DeviceInfo> out;
    VkApi api;
    std::string w;
    if (!loadVulkanLoader(api, &w)) { if (why) *why = w; return out; }
    VkInstance inst = VK_NULL_HANDLE;
    if (!makeInstance(api, inst, &w)) { if (why) *why = w; return out; }
    std::vector<VkPhysicalDevice> phys;
    describeDevices(api, inst, phys, out);
    api.vkDestroyInstance(inst, nullptr);
    if (out.empty() && why) *why = "the Vulkan loader found no device (no graphics driver with Vulkan support)";
    return out;
}

bool ready(std::string* why)
{
    std::string w;
    std::shared_ptr<Core> c = acquireCore(&w);
    if (!c && why) *why = w;
    return c != nullptr;
}

bool deviceLost()
{
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.core && g.core->lost;
}

std::string status()
{
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.enabled) return "Vulkan: off";
    if (g.core) {
        if (g.core->lost) return "Vulkan: device lost (" + g.core->info.name + "), effects use the CPU";
        return "Vulkan: " + g.core->info.name + (g.core->info.software ? " (software)" : "");
    }
    if (g.tried) return "Vulkan: not available (" + g.failure + "), effects use the CPU";
    return "Vulkan: not started yet";
}

DeviceInfo currentDevice()
{
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.core ? g.core->info : DeviceInfo();
}

void shutdown()
{
    Global& g = G();
    std::shared_ptr<Core> old;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        old = std::move(g.core);
        g.tried = false;
    }
    if (old) destroyCore(*old);
}

std::vector<std::string> shaderNames()
{
    std::vector<std::string> v;
    for (const ShaderBlob* b : allShaderBlobs()) v.push_back(b->name);
    return v;
}
bool hasShader(const std::string& name) { return findShaderBlob(name) != nullptr; }

void dropResident()
{
    Global& g = G();
    std::shared_ptr<Core> c;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        c = g.core;
    }
    // (every thread's kept frame stops matching; each frees its buffer at its next run)
    if (c) c->residentEpoch++;
}

// ---------------------------------------------------------------------------------------------------- public: run
namespace {
std::atomic<uint64_t> gRuns(0), gStayed(0), gFailed(0), gDeferred(0);
std::atomic<int> gChainDefer(-1);      // -1: not asked yet
bool runInner(const Plan& plan, FrameIO& io, RunStats* stats);
// Why the last run() on this thread failed (for reno_cli vkcheck): the check that stopped it.
thread_local std::string tRunWhy;
bool runFailed(const char* why)
{
    tRunWhy = why;
    return false;
}
}

void setChainDeferral(bool on) { gChainDefer = on ? 1 : 0; }

Counters counters()
{
    Counters c;
    c.runs = gRuns.load();
    c.inputsStayed = gStayed.load();
    c.failed = gFailed.load();
    c.deferred = gDeferred.load();
    return c;
}

std::string lastRunFailure() { return tRunWhy; }

bool run(const Plan& plan, FrameIO& io, RunStats* stats)
{
    RunStats local;
    RunStats* st = stats ? stats : &local;
    tRunWhy.clear();
    const bool ok = runInner(plan, io, st);
    if (ok) {
        gRuns++;
        if (st->inputStayed) gStayed++;
        if (st->deferred) gDeferred++;
    } else {
        gFailed++;
    }
    return ok;
}

namespace {
bool runInner(const Plan& plan, FrameIO& io, RunStats* stats)
{
    const Clock::time_point t0 = Clock::now();
    std::shared_ptr<Core> cp = acquireCore(nullptr);
    if (!cp) return runFailed("!cp");
    Core& c = *cp;
    const VkApi& a = c.api;
    if (c.lost) return runFailed("c.lost");
    if (plan.w != io.w || plan.h != io.h || io.w < 1 || io.h < 1 || (!io.in && !io.noInput) || !io.out || plan.bufs.size() < 2 || plan.passes.empty()) return runFailed("plan.w != io.w || plan.h != io.h || io.w < 1 || io.h < 1 || (!io.in && !io.noInput) || !io.out || plan.bufs.si");
    if (plan.bufs[0].w != io.w || plan.bufs[0].h != io.h || plan.bufs[0].comps != 4 || plan.bufs[1].w != io.w || plan.bufs[1].h != io.h || plan.bufs[1].comps != 4) return runFailed("plan.bufs[0].w != io.w || plan.bufs[0].h != io.h || plan.bufs[0].comps != 4 || plan.bufs[1].w != io.w || plan.");
    const size_t bpp = io.isFloat ? 16 : 4;
    const size_t rawBytes = (size_t)io.w * io.h * bpp;
    if (rawBytes > c.info.maxStorageRange) return runFailed("rawBytes > c.info.maxStorageRange");
    for (const BufDesc& b : plan.bufs) if (b.w == 0 || b.h == 0 || b.comps == 0 || b.bytes() > c.info.maxStorageRange) return runFailed("b.w == 0 || b.h == 0 || b.comps == 0 || b.bytes() > c.info.maxStorageRange");
    for (const Pass& p : plan.passes) {
        if (p.pushSize > kMaxPush || p.pushSize > c.info.maxPushConstants) return runFailed("p.pushSize > kMaxPush || p.pushSize > c.info.maxPushConstants");
        if (p.in0 >= (int)plan.bufs.size() || p.in1 >= (int)plan.bufs.size() || p.out < 0 || p.out >= (int)plan.bufs.size() || p.in0 < -1 || p.in1 < -1) return runFailed("p.in0 >= (int)plan.bufs.size() || p.in1 >= (int)plan.bufs.size() || p.out < 0 || p.out >= (int)plan.bufs.size(");
        if (p.table >= (int)plan.tables.size() || p.table < -1 || p.threadsX == 0 || p.threadsY == 0) return runFailed("p.table >= (int)plan.tables.size() || p.table < -1 || p.threadsX == 0 || p.threadsY == 0");
        if (!hasShader(p.shader)) return runFailed("!hasShader(p.shader)");
    }
    if (!hasShader(io.isFloat ? "unpack_f32" : "unpack_u8") || !hasShader(io.isFloat ? "pack_f32" : "pack_u8")) return runFailed("!hasShader(io.isFloat ? \"unpack_f32\" : \"unpack_u8\") || !hasShader(io.isFloat ? \"pack_f32\" : \"pack_u8\")");
    if (plan.passes.size() + 2 > kMaxPasses) return runFailed("plan.passes.size() + 2 > kMaxPasses");

    ThreadState* ts = stateFor(c);
    if (!ts) return runFailed("!ts");

    // ---- buffers
    std::vector<Buffer> held;
    Buffer stageUp, stageDown, tableBuf;
    bool haveStageUp = false, haveStageDown = false, haveTable = false;
    struct Release {
        Core& c; std::vector<Buffer>& held; Buffer &up, &down, &tbl; bool &hu, &hd, &ht;
        ~Release()
        {
            for (Buffer& b : held) giveBuffer(c, b);
            if (hu) giveBuffer(c, up);
            if (hd) giveBuffer(c, down);
            if (ht) giveBuffer(c, tbl);
        }
    } release{c, held, stageUp, stageDown, tableBuf, haveStageUp, haveStageDown, haveTable};

    // a frame the previous effect of the chain left on the card without downloading it (FrameIO::deferOut)
    Buffer pendIn;
    bool usePending = false;
    if (!io.noInput && io.inSerial != 0) {
        std::lock_guard<std::mutex> lk(c.mu);
        const auto it = c.pending.find(io.inSerial);
        if (it != c.pending.end() && it->second.w == io.w && it->second.h == io.h && it->second.isFloat == io.isFloat && it->second.bgra == io.bgra) {
            pendIn = it->second.buf;
            usePending = true;
        }
    }
    bool defer = false;
    if (gChainDefer.load() < 0) gChainDefer = std::getenv("RENO_VK_NO_DEFER") ? 0 : 1;
    if (io.deferOut && io.outSerial != 0 && gChainDefer.load() == 1) {
        std::lock_guard<std::mutex> lk(c.mu);
        defer = c.pending.size() < kMaxPending;
    }
    const bool reuseInput = usePending || (!io.noInput && io.inSerial != 0 && ts->resident.serial == io.inSerial && ts->resident.epoch == c.residentEpoch.load() && ts->resident.buf.buf && ts->resident.w == io.w &&
                            ts->resident.h == io.h && ts->resident.isFloat == io.isFloat && ts->resident.bgra == io.bgra &&
                            ts->resident.hash == sampleHash(io.in, io.inRowBytes, io.w, io.h, bpp));
    Buffer rawIn;
    bool rawInIsResident = false;
    if (usePending) {
        rawIn = pendIn;
    } else if (reuseInput) {
        rawIn = ts->resident.buf;
        rawInIsResident = true;
    } else {
        if (!takeBuffer(c, rawBytes, Kind::Device, rawIn)) return runFailed("!takeBuffer(c, rawBytes, Kind::Device, rawIn)");
        held.push_back(rawIn);
    }
    Buffer rawOut;
    if (!takeBuffer(c, rawBytes, Kind::Device, rawOut)) {
        if (rawInIsResident) { giveBuffer(c, ts->resident.buf); ts->resident = Resident(); }
        return runFailed("line 940");
    }
    std::vector<Buffer> frameBufs(plan.bufs.size());
    bool okAlloc = true;
    for (size_t k = 0; k < plan.bufs.size() && okAlloc; ++k) {
        okAlloc = takeBuffer(c, plan.bufs[k].bytes(), Kind::Device, frameBufs[k]);
        if (okAlloc) held.push_back(frameBufs[k]);
    }
    size_t tableBytes = 0;
    std::vector<size_t> tableOff(plan.tables.size(), 0);
    for (size_t k = 0; k < plan.tables.size(); ++k) {
        tableOff[k] = tableBytes;
        tableBytes += alignUp(std::max<size_t>(plan.tableFloats(k) * sizeof(float), 16), kAlign);
    }
    // a frame table's 8-bit frame goes up compactly, after the tables' own floats; the card turns it into the table's first floats
    int frameTab = -1;
    for (size_t k = 0; k < plan.tables.size(); ++k) if (plan.tableFrames[k].bytes) frameTab = (int)k;
    const size_t frame8Bytes = frameTab >= 0 ? (size_t)io.w * io.h * 4 : 0;
    if (frameTab >= 0 && !(hasShader("unpack_u8"))) return runFailed("no unpack_u8");
    std::vector<size_t> tailOff(plan.tables.size(), 0);          // where a table's own floats are in the staging memory
    size_t tailBytes = 0;
    for (size_t k = 0; k < plan.tables.size(); ++k) {
        tailOff[k] = tailBytes;
        tailBytes += alignUp(plan.tables[k].size() * sizeof(float), 16);
    }
    if (okAlloc && tableBytes) {
        okAlloc = tableBytes <= c.info.maxStorageRange && takeBuffer(c, tableBytes, Kind::Device, tableBuf);
        haveTable = okAlloc;
    }
    const bool needFrameUpload = !reuseInput && !io.noInput;
    const size_t upBytes = (needFrameUpload ? rawBytes : 0) + tailBytes + frame8Bytes;
    Buffer frame8Dev;
    if (okAlloc && frameTab >= 0) { okAlloc = takeBuffer(c, frame8Bytes, Kind::Device, frame8Dev); if (okAlloc) held.push_back(frame8Dev); }
    if (okAlloc && upBytes) { okAlloc = takeBuffer(c, upBytes, Kind::StageUp, stageUp); haveStageUp = okAlloc; }
    if (okAlloc && !defer) { okAlloc = takeBuffer(c, rawBytes, Kind::StageDown, stageDown); haveStageDown = okAlloc; }
    if (!okAlloc) {
        giveBuffer(c, rawOut);
        if (rawInIsResident) { giveBuffer(c, ts->resident.buf); ts->resident = Resident(); }
        return runFailed("line 965");
    }
    struct OutRelease { Core& c; Buffer& b; bool keep; ~OutRelease() { if (!keep) giveBuffer(c, b); } } outRelease{c, rawOut, false};

    // ---- fill the staging memory
    const Clock::time_point tFill = Clock::now();
    if (needFrameUpload) {
        uint8_t* dst = (uint8_t*)stageUp.mapped;
        const size_t rb = (size_t)io.w * bpp;
        if (io.inRowBytes == rb) std::memcpy(dst, io.in, rb * io.h);
        else for (uint32_t y = 0; y < io.h; ++y) std::memcpy(dst + rb * y, io.in + io.inRowBytes * y, rb);
    }
    const size_t tailBase = needFrameUpload ? rawBytes : 0;
    {
        uint8_t* base = (uint8_t*)stageUp.mapped + tailBase;
        for (size_t k = 0; k < plan.tables.size(); ++k)
            if (!plan.tables[k].empty()) std::memcpy(base + tailOff[k], plan.tables[k].data(), plan.tables[k].size() * sizeof(float));
        if (frameTab >= 0) {
            const Plan::FrameTable& ft = plan.tableFrames[(size_t)frameTab];
            uint8_t* d8 = base + tailBytes;
            const size_t rb = (size_t)io.w * 4;
            if (ft.rowBytes == rb) std::memcpy(d8, ft.bytes, rb * io.h);
            else for (uint32_t y = 0; y < io.h; ++y) std::memcpy(d8 + rb * y, ft.bytes + ft.rowBytes * y, rb);
        }
    }
    if (haveStageUp && !stageUp.coherent) {
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, stageUp.mem, 0, VK_WHOLE_SIZE};
        a.vkFlushMappedMemoryRanges(c.dev, 1, &mr);
    }

    const Clock::time_point tFilled = Clock::now();
    // ---- the passes, with the buffers they use
    std::vector<Bound> bound;
    auto dummyInfo = [&](Bound& b, int k) { b.buf[k] = c.dummy.buf; b.off[k] = 0; b.range[k] = c.dummy.size; };
    if (!io.noInput) {
        Bound u;
        u.shader = io.isFloat ? "unpack_f32" : "unpack_u8";
        for (int k = 0; k < kBindings; ++k) dummyInfo(u, k);
        u.buf[0] = rawIn.buf; u.range[0] = rawBytes;
        u.buf[2] = frameBufs[0].buf; u.range[2] = plan.bufs[0].bytes();
        UnpackPush up = {io.w, io.h, io.bgra ? 1u : 0u, 0};
        std::memcpy(u.push, &up, sizeof up);
        u.pushSize = sizeof up;
        u.tx = io.w; u.ty = io.h;
        bound.push_back(u);
    }
    if (frameTab >= 0) {
        Bound u;
        u.shader = "unpack_u8";
        for (int k = 0; k < kBindings; ++k) dummyInfo(u, k);
        u.buf[0] = frame8Dev.buf; u.range[0] = frame8Bytes;
        u.buf[2] = tableBuf.buf; u.off[2] = tableOff[(size_t)frameTab]; u.range[2] = (VkDeviceSize)io.w * io.h * 16;
        UnpackPush up = {io.w, io.h, plan.tableFrames[(size_t)frameTab].bgra ? 1u : 0u, 0};
        std::memcpy(u.push, &up, sizeof up);
        u.pushSize = sizeof up;
        u.tx = io.w; u.ty = io.h;
        bound.push_back(u);
    }
    for (const Pass& p : plan.passes) {
        Bound b;
        b.shader = p.shader;
        for (int k = 0; k < kBindings; ++k) dummyInfo(b, k);
        if (p.in0 >= 0) { b.buf[0] = frameBufs[(size_t)p.in0].buf; b.range[0] = plan.bufs[(size_t)p.in0].bytes(); }
        if (p.in1 >= 0) { b.buf[1] = frameBufs[(size_t)p.in1].buf; b.range[1] = plan.bufs[(size_t)p.in1].bytes(); }
        b.buf[2] = frameBufs[(size_t)p.out].buf; b.range[2] = plan.bufs[(size_t)p.out].bytes();
        if (p.table >= 0) {
            b.buf[3] = tableBuf.buf;
            b.off[3] = tableOff[(size_t)p.table];
            b.range[3] = alignUp(std::max<size_t>(plan.tableFloats((size_t)p.table) * sizeof(float), 16), kAlign);
        }
        std::memcpy(b.push, p.push, p.pushSize);
        b.pushSize = p.pushSize;
        b.tx = p.threadsX; b.ty = p.threadsY;
        bound.push_back(b);
    }
    {
        Bound k;
        k.shader = io.isFloat ? "pack_f32" : "pack_u8";
        for (int j = 0; j < kBindings; ++j) dummyInfo(k, j);
        k.buf[0] = frameBufs[1].buf; k.range[0] = plan.bufs[1].bytes();
        k.buf[2] = rawOut.buf; k.range[2] = rawBytes;
        UnpackPush up = {io.w, io.h, io.bgra ? 1u : 0u, 0};
        std::memcpy(k.push, &up, sizeof up);
        k.pushSize = sizeof up;
        k.tx = io.w; k.ty = io.h;
        bound.push_back(k);
    }
    std::vector<VkPipeline> pipes(bound.size());
    std::vector<std::pair<uint32_t, uint32_t>> locals(bound.size());
    for (size_t k = 0; k < bound.size(); ++k) {
        const auto cached = ts->pipes.find(bound[k].shader);
        if (cached != ts->pipes.end()) {
            pipes[k] = cached->second.pipe;
            locals[k] = std::make_pair(cached->second.lx, cached->second.ly);
            continue;
        }
        pipes[k] = pipelineFor(c, bound[k].shader, locals[k].first, locals[k].second);
        if (pipes[k]) ts->pipes[bound[k].shader] = PipeInfo{pipes[k], locals[k].first, locals[k].second};
        if (!pipes[k]) { if (c.lost) return runFailed("!pipes[k]) { if (c.lost"); return runFailed("!pipes[k]) { if (c.lost"); }
    }

    // ---- record
    a.vkResetCommandPool(c.dev, ts->pool, 0);
    // descriptor sets are kept from frame to frame (same buffers, same sets); a destroyed buffer, or a pool that would run out, starts them afresh
    if (ts->setsGen != c.bufGen.load() || ts->sets.size() + bound.size() > kMaxPasses) {
        a.vkResetDescriptorPool(c.dev, ts->dpool, 0);
        ts->sets.clear();
        ts->setsGen = c.bufGen.load();
    }
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (a.vkBeginCommandBuffer(ts->cb, &bi) != VK_SUCCESS) return runFailed("a.vkBeginCommandBuffer(ts->cb, &bi) != VK_SUCCESS");
    VkCommandBuffer cb = ts->cb;
    const bool useTs = ts->queries != VK_NULL_HANDLE;
    if (useTs) a.vkCmdResetQueryPool(cb, ts->queries, 0, 4);
    if (useTs) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts->queries, 2);
    auto barrier = [&](VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags src, VkAccessFlags dst) {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, src, dst};
        a.vkCmdPipelineBarrier(cb, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    if (needFrameUpload) {
        VkBufferCopy cr = {0, 0, rawBytes};
        a.vkCmdCopyBuffer(cb, stageUp.buf, rawIn.buf, 1, &cr);
    }
    if (haveTable) {
        // each table's own floats to its place (after the frame floats the card makes, for a frame table)
        std::vector<VkBufferCopy> crs;
        for (size_t k = 0; k < plan.tables.size(); ++k) {
            if (plan.tables[k].empty()) continue;
            crs.push_back(VkBufferCopy{tailBase + tailOff[k], tableOff[k] + plan.frameFloats(k) * sizeof(float), plan.tables[k].size() * sizeof(float)});
        }
        if (!crs.empty()) a.vkCmdCopyBuffer(cb, stageUp.buf, tableBuf.buf, (uint32_t)crs.size(), crs.data());
    }
    if (frameTab >= 0) {
        VkBufferCopy cr = {tailBase + tailBytes, 0, frame8Bytes};
        a.vkCmdCopyBuffer(cb, stageUp.buf, frame8Dev.buf, 1, &cr);
    }
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (useTs) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, ts->queries, 0);
    bool recordOk = true;
    for (size_t k = 0; k < bound.size() && recordOk; ++k) {
        const Bound& b = bound[k];
        DescKey dk;
        for (int j = 0; j < kBindings; ++j) { dk.buf[j] = b.buf[j]; dk.off[j] = b.off[j]; dk.range[j] = b.range[j]; }
        VkDescriptorSet set = VK_NULL_HANDLE;
        const auto have = ts->sets.find(dk);
        if (have != ts->sets.end()) {
            set = have->second;
        } else {
            VkDescriptorSetAllocateInfo ai = {};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = ts->dpool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &c.dsl;
            if (a.vkAllocateDescriptorSets(c.dev, &ai, &set) != VK_SUCCESS) { recordOk = false; break; }
            VkDescriptorBufferInfo infos[kBindings];
            VkWriteDescriptorSet wr[kBindings] = {};
            for (int j = 0; j < kBindings; ++j) {
                infos[j] = {b.buf[j], b.off[j], b.range[j]};
                wr[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[j].dstSet = set;
                wr[j].dstBinding = (uint32_t)j;
                wr[j].descriptorCount = 1;
                wr[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr[j].pBufferInfo = &infos[j];
            }
            a.vkUpdateDescriptorSets(c.dev, kBindings, wr, 0, nullptr);
            ts->sets.emplace(dk, set);
        }
        a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[k]);
        a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0, 1, &set, 0, nullptr);
        if (b.pushSize) a.vkCmdPushConstants(cb, c.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, b.pushSize, b.push);
        const uint32_t gx = (b.tx + locals[k].first - 1) / locals[k].first, gy = (b.ty + locals[k].second - 1) / locals[k].second;
        if (gx > c.limits.maxComputeWorkGroupCount[0] || gy > c.limits.maxComputeWorkGroupCount[1]) { recordOk = false; break; }
        a.vkCmdDispatch(cb, gx, gy, 1);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    if (!recordOk) { a.vkEndCommandBuffer(cb); return runFailed("!recordOk"); }
    if (useTs) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, ts->queries, 1);
    if (!defer) {
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy cr = {0, 0, rawBytes};
        a.vkCmdCopyBuffer(cb, rawOut.buf, stageDown.buf, 1, &cr);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    if (useTs) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts->queries, 3);
    if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) return runFailed("a.vkEndCommandBuffer(cb) != VK_SUCCESS");
    const Clock::time_point tRecorded = Clock::now();

    // ---- go
    a.vkResetFences(c.dev, 1, &ts->fence);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(c.queueMu);
        r = a.vkQueueSubmit(c.queue, 1, &si, ts->fence);
    }
    if (r != VK_SUCCESS) {
        if (r == VK_ERROR_DEVICE_LOST) c.lost = true;
        return runFailed("line 1114");
    }
    r = a.vkWaitForFences(c.dev, 1, &ts->fence, VK_TRUE, (uint64_t)20 * 1000 * 1000 * 1000);
    if (r != VK_SUCCESS) {
        // device lost, or a pass that never ends: the card is not trusted any more
        if (!c.lost) logf("Vulkan: device did not finish (%d); built-in effects will render on the CPU", (int)r);
        c.lost = true;
        return runFailed("line 1121");
    }
    const Clock::time_point tWaited = Clock::now();
    if (!defer && !stageDown.coherent) {
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, stageDown.mem, 0, VK_WHOLE_SIZE};
        a.vkInvalidateMappedMemoryRanges(c.dev, 1, &mr);
    }
    if (!defer) {
        const uint8_t* src = (const uint8_t*)stageDown.mapped;
        const size_t rb = (size_t)io.w * bpp;
        if (io.outRowBytes == rb) std::memcpy(io.out, src, rb * io.h);
        else for (uint32_t y = 0; y < io.h; ++y) std::memcpy(io.out + io.outRowBytes * y, src + rb * y, rb);
    }
    const Clock::time_point tRead = Clock::now();
    if (stats) {
        stats->gpuMs = 0;
        stats->copyUpMs = stats->copyDownMs = 0;
        stats->fillMs = std::chrono::duration<double, std::milli>(tFilled - tFill).count();
        stats->recordMs = std::chrono::duration<double, std::milli>(tRecorded - tFilled).count();
        stats->waitMs = std::chrono::duration<double, std::milli>(tWaited - tRecorded).count();
        stats->readMs = std::chrono::duration<double, std::milli>(tRead - tWaited).count();
        if (useTs) {
            uint64_t t[4] = {0, 0, 0, 0};
            if (a.vkGetQueryPoolResults(c.dev, ts->queries, 0, 4, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && t[1] >= t[0]) {
                stats->gpuMs = (double)(t[1] - t[0]) * c.timestampPeriod * 1e-6;
                if (t[0] >= t[2]) stats->copyUpMs = (double)(t[0] - t[2]) * c.timestampPeriod * 1e-6;
                if (t[3] >= t[1]) stats->copyDownMs = (double)(t[3] - t[1]) * c.timestampPeriod * 1e-6;
            }
        }
        stats->inputStayed = reuseInput;
        stats->deferred = defer;
        stats->passes = (int)bound.size();
    }

    // ---- the packed result stays for the next effect of a chain
    if (rawInIsResident) {
        giveBuffer(c, ts->resident.buf);
        ts->resident = Resident();
    } else if (ts->resident.buf.buf) {
        giveBuffer(c, ts->resident.buf);
        ts->resident = Resident();
    }
    if (defer) {
        // the card's copy is the frame now; whoever holds pendingHold(outSerial) frees it
        PendingFrame pf;
        pf.buf = rawOut;
        pf.w = io.w; pf.h = io.h; pf.isFloat = io.isFloat; pf.bgra = io.bgra;
        {
            std::lock_guard<std::mutex> lk(c.mu);
            c.pending[io.outSerial] = pf;
        }
        outRelease.keep = true;
        rawOut = Buffer();
    } else if (io.outSerial != 0) {
        ts->resident.buf = rawOut;
        ts->resident.serial = io.outSerial;
        ts->resident.epoch = c.residentEpoch.load();
        ts->resident.w = io.w;
        ts->resident.h = io.h;
        ts->resident.isFloat = io.isFloat;
        ts->resident.bgra = io.bgra;
        ts->resident.hash = sampleHash(io.out, io.outRowBytes, io.w, io.h, bpp);
        outRelease.keep = true;
        rawOut = Buffer();
    }
    if (stats) stats->totalMs = msSince(t0);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------- frames left on the card
namespace {
thread_local bool tDefer = false;
std::shared_ptr<Core> currentCore()
{
    Global& g = G();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.core;
}
void syncHook(const Image& im) { materialize(im.serial, const_cast<uint8_t*>(im.data.get()), im.rowBytes); }
struct HookInit { HookInit() { gImageSyncHook = &syncHook; } } gHookInit;
}  // namespace

DeferOutput::DeferOutput(bool on) : prev_(tDefer) { tDefer = on; }
DeferOutput::~DeferOutput() { tDefer = prev_; }
bool deferOutputRequested() { return tDefer; }

bool isPending(uint64_t serial)
{
    std::shared_ptr<Core> cp = currentCore();
    if (!cp || serial == 0) return false;
    std::lock_guard<std::mutex> lk(cp->mu);
    return cp->pending.count(serial) != 0;
}

std::shared_ptr<void> pendingHold(uint64_t serial)
{
    std::shared_ptr<Core> cp = currentCore();
    if (!cp || !isPending(serial)) return nullptr;
    return std::shared_ptr<void>(new uint64_t(serial), [cp](void* p) {
        const uint64_t s = *(uint64_t*)p;
        delete (uint64_t*)p;
        PendingFrame pf;
        {
            std::lock_guard<std::mutex> lk(cp->mu);
            const auto it = cp->pending.find(s);
            if (it == cp->pending.end()) return;
            pf = it->second;
            cp->pending.erase(it);
        }
        giveBuffer(*cp, pf.buf);
    });
}

bool materialize(uint64_t serial, uint8_t* dst, size_t rowBytes)
{
    std::shared_ptr<Core> cp = currentCore();
    if (!cp || !dst || serial == 0) return false;
    Core& c = *cp;
    const VkApi& a = c.api;
    PendingFrame pf;
    {
        std::lock_guard<std::mutex> lk(c.mu);
        const auto it = c.pending.find(serial);
        if (it == c.pending.end()) return false;
        pf = it->second;
    }
    if (c.lost) return false;
    ThreadState* ts = stateFor(c);
    if (!ts) return false;
    const size_t bpp = pf.isFloat ? 16 : 4, rb = (size_t)pf.w * bpp, bytes = rb * pf.h;
    Buffer down;
    if (!takeBuffer(c, bytes, Kind::StageDown, down)) return false;
    struct Give { Core& c; Buffer& b; ~Give() { giveBuffer(c, b); } } give{c, down};
    a.vkResetCommandPool(c.dev, ts->pool, 0);
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (a.vkBeginCommandBuffer(ts->cb, &bi) != VK_SUCCESS) return false;
    VkBufferCopy cr = {0, 0, bytes};
    a.vkCmdCopyBuffer(ts->cb, pf.buf.buf, down.buf, 1, &cr);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    a.vkCmdPipelineBarrier(ts->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    if (a.vkEndCommandBuffer(ts->cb) != VK_SUCCESS) return false;
    a.vkResetFences(c.dev, 1, &ts->fence);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &ts->cb;
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(c.queueMu);
        r = a.vkQueueSubmit(c.queue, 1, &si, ts->fence);
    }
    if (r != VK_SUCCESS) { if (r == VK_ERROR_DEVICE_LOST) c.lost = true; return false; }
    if (a.vkWaitForFences(c.dev, 1, &ts->fence, VK_TRUE, (uint64_t)20 * 1000 * 1000 * 1000) != VK_SUCCESS) { c.lost = true; return false; }
    if (!down.coherent) {
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, down.mem, 0, VK_WHOLE_SIZE};
        a.vkInvalidateMappedMemoryRanges(c.dev, 1, &mr);
    }
    const uint8_t* src = (const uint8_t*)down.mapped;
    if (rowBytes == rb) std::memcpy(dst, src, bytes);
    else for (uint32_t y = 0; y < pf.h; ++y) std::memcpy(dst + rowBytes * y, src + rb * y, rb);
    return true;
}

}  // namespace gpu
}  // namespace reno
