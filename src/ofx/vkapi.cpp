#include "vkapi.h"
#include "../core/dynlib.h"
#include <mutex>

namespace reno {
namespace gpu {

namespace {
DynLib& loaderLib()
{
    static DynLib lib;                  // (never closed: drivers do not like to be unloaded while their threads are still going)
    return lib;
}
std::mutex gLoaderMutex;
PFN_vkGetInstanceProcAddr gGetInstanceProcAddr = nullptr;
std::string gLoaderError;
bool gLoaderTried = false;
}  // namespace

bool loadVulkanLoader(VkApi& api, std::string* why)
{
    std::lock_guard<std::mutex> lk(gLoaderMutex);
    if (!gLoaderTried) {
        gLoaderTried = true;
#ifdef _WIN32
        const char* names[] = {"vulkan-1.dll", nullptr};
#elif defined(__APPLE__)
        const char* names[] = {"libvulkan.1.dylib", "libMoltenVK.dylib", nullptr};
#else
        const char* names[] = {"libvulkan.so.1", "libvulkan.so", nullptr};
#endif
        std::string err;
        for (int k = 0; names[k]; ++k) {
            std::string e;
            if (loaderLib().open(names[k], &e)) {
                gGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)loaderLib().symbol("vkGetInstanceProcAddr");
                if (gGetInstanceProcAddr) break;
                err = std::string(names[k]) + " has no vkGetInstanceProcAddr";
                loaderLib().close();
            } else {
                err = std::string("cannot load ") + names[k] + (e.empty() ? std::string() : " (" + e + ")");
            }
        }
        if (!gGetInstanceProcAddr) gLoaderError = err.empty() ? std::string("no Vulkan loader found") : err;
    }
    if (!gGetInstanceProcAddr) {
        if (why) *why = gLoaderError;
        return false;
    }
    api.vkGetInstanceProcAddr = gGetInstanceProcAddr;
#define X(name) api.name = (PFN_##name)gGetInstanceProcAddr(nullptr, #name);
    RENO_VK_GLOBAL_FUNCS(X)
#undef X
    if (!api.vkCreateInstance) {
        if (why) *why = "the Vulkan loader lacks vkCreateInstance";
        return false;
    }
    return true;
}

bool loadInstanceFunctions(VkApi& api, VkInstance instance, std::string* why)
{
    bool ok = true;
    std::string missing;
#define X(name) api.name = (PFN_##name)api.vkGetInstanceProcAddr(instance, #name); if (!api.name) { ok = false; missing += std::string(" ") + #name; }
    RENO_VK_INSTANCE_FUNCS(X)
#undef X
    if (!ok && why) *why = "missing Vulkan functions:" + missing;
    return ok;
}

bool loadDeviceFunctions(VkApi& api, VkInstance instance, VkDevice device, std::string* why)
{
    (void)instance;
    bool ok = true;
    std::string missing;
#define X(name) api.name = (PFN_##name)api.vkGetDeviceProcAddr(device, #name); if (!api.name) { ok = false; missing += std::string(" ") + #name; }
    RENO_VK_DEVICE_FUNCS(X)
#undef X
    if (!ok && why) *why = "missing Vulkan device functions:" + missing;
    return ok;
}

}  // namespace gpu
}  // namespace reno
