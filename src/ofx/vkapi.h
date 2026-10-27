// The Vulkan functions Reno uses, loaded at run time: the loader library (vulkan-1.dll / libvulkan.so.1) is opened with LoadLibrary / dlopen and every
// function fetched through vkGetInstanceProcAddr / vkGetDeviceProcAddr (as volk does), so there is nothing to link or install. No loader or driver is not
// an error: effects are rendered on the CPU.
#pragma once
#define VK_NO_PROTOTYPES
#include "../../third_party/vulkan/vulkan/vulkan_core.h"
#include <string>

namespace reno {
namespace gpu {

// Functions taken from the loader itself or with a null instance.
#define RENO_VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties)

// Functions fetched with the instance.
#define RENO_VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

// Instance functions that may be missing (Vulkan 1.1 and later, or an extension): used when they are there.
#define RENO_VK_INSTANCE_OPT_FUNCS(X) \
    X(vkGetPhysicalDeviceProperties2)

// Functions fetched with the device (one table for the device Reno works with).
#define RENO_VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkFlushMappedMemoryRanges) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCreatePipelineCache) \
    X(vkGetPipelineCacheData) \
    X(vkDestroyPipelineCache) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdDispatch) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBuffer) \
    X(vkCmdFillBuffer) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkResetFences) \
    X(vkWaitForFences) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) \
    X(vkGetQueryPoolResults)

struct VkApi {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define X(name) PFN_##name name = nullptr;
    RENO_VK_GLOBAL_FUNCS(X)
    RENO_VK_INSTANCE_FUNCS(X)
    RENO_VK_INSTANCE_OPT_FUNCS(X)
    RENO_VK_DEVICE_FUNCS(X)
#undef X
    uint32_t instanceApiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);       // what the instance was made with
};

// Opens the Vulkan loader library and takes the global functions from it. False (with 'why' set) when there is no loader.
// Safe to call many times; the library stays open for the life of the program.
bool loadVulkanLoader(VkApi& api, std::string* why);
// Takes the instance functions (after vkCreateInstance).
bool loadInstanceFunctions(VkApi& api, VkInstance instance, std::string* why);
// Takes the device functions (after vkCreateDevice).
bool loadDeviceFunctions(VkApi& api, VkInstance instance, VkDevice device, std::string* why);

}  // namespace gpu
}  // namespace reno
