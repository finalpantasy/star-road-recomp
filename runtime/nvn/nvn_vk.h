// Star Road Recomp: NVN on Vulkan, Vulkan bring-up (task-12). Our own code (D-008).
// Dynamic loading of vulkan-1.dll, one device and one queue, a block sub-allocator, a timeline semaphore for GPU
// completion. No Vulkan symbol is linked: every entry point comes through vkGetInstanceProcAddr.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nvn {

// Instance level entry points (loaded with the instance).
#define NVN_VK_INSTANCE_FNS(X) \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
  X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) \
  X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
  X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT) X(vkGetPhysicalDeviceImageFormatProperties)

// Device level entry points.
#define NVN_VK_DEVICE_FNS(X) \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit2) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkAllocateMemory) X(vkFreeMemory) \
  X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
  X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
  X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
  X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
  X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
  X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkDestroyPipelineCache) \
  X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkSignalSemaphore) X(vkCreateFence) \
  X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
  X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) \
  X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) \
  X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdSetDepthBounds) \
  X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) \
  X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) \
  X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) \
  X(vkCmdPipelineBarrier2) X(vkCmdPushConstants) X(vkCmdBeginDebugUtilsLabelEXT) X(vkCmdEndDebugUtilsLabelEXT) \
  X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
  X(vkGetMemoryHostPointerPropertiesEXT) X(vkGetImageSubresourceLayout)

#define NVN_VK_DECL(n) PFN_##n n;
struct VkFns {
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
  PFN_vkCreateInstance vkCreateInstance;
  PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
  PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;
  NVN_VK_INSTANCE_FNS(NVN_VK_DECL)
  NVN_VK_DEVICE_FNS(NVN_VK_DECL)
};
#undef NVN_VK_DECL

struct VkInitOptions {
  bool validation = false;      // VK_LAYER_KHRONOS_validation (layer path from VK_LAYER_PATH or `layerPath`)
  std::string layerPath;        // directory holding VkLayer_khronos_validation.json
  bool surfaceExtensions = false;  // enable VK_KHR_surface and win32 surface (hidden window presenting)
  int deviceIndex = -1;         // -1: prefer a discrete GPU
};

// One block of device memory sub-allocated by a first-fit free list.
struct MemBlock;
struct MemAlloc {
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize offset = 0, size = 0;
  void* mapped = nullptr;       // non-null for host visible blocks (already offset)
  MemBlock* block = nullptr;
};

struct Vk {
  VkFns f{};
  HMODULE lib = nullptr;
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties memProps{};
  VkPhysicalDeviceFeatures2 feat{};
  VkPhysicalDeviceVulkan11Features f11{};
  VkPhysicalDeviceVulkan12Features f12{};
  VkPhysicalDeviceVulkan13Features f13{};
  VkPhysicalDeviceVulkan11Properties p11{};   // subgroup size, stages, operations (shader cache capability check)
  VkPhysicalDeviceVulkan13Properties p13{};   // required subgroup size range
  VkPhysicalDeviceDepthClipControlFeaturesEXT depthClip{};
  VkPhysicalDevice4444FormatsFeaturesEXT f4444{};
  VkPhysicalDeviceIndexTypeUint8FeaturesEXT idx8{};
  VkPhysicalDeviceCustomBorderColorFeaturesEXT customBorder{};
  VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR vertexDivisor{};
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  bool hasHostImport = false, hasDepthClipControl = false, hasSwapchain = false, has4444 = false, hasIdx8 = false,
       hasCustomBorder = false, hasDebugUtils = false, hasMinMax = false;
  VkDeviceSize hostAlign = 4096;
  uint32_t maxVertexDivisor = 1;
  VkDeviceSize granularity = 1;
  VkSemaphore timeline = VK_NULL_HANDLE;   // GPU completion counter (monotonic)
  std::recursive_mutex lock;               // the one NVN lock (object creation, queue work)
  // validation message counters (the tests assert zero)
  std::atomic<int> valErrors{0}, valWarnings{0};
  std::string firstValError;
  // Validation messages grouped by message id (errors and warnings), for test reports: id -> count and the first full message
  struct ValTally { int count = 0; bool error = false; std::string first; };
  std::mutex valLock; std::map<std::string, ValTally> valById;
  bool inited = false;
};

extern Vk g_vk;
inline VkFns& F() { return g_vk.f; }
inline VkDevice D() { return g_vk.dev; }

bool VkBringUp(const VkInitOptions& opt);   // idempotent; false with a message in LastVkError()
void VkTearDown();
const std::string& LastVkError();

// ---- memory ------------------------------------------------------------------------------------------------
enum class MemKind { Device, HostVisible };
bool AllocMemory(MemKind kind, const VkMemoryRequirements& req, MemAlloc* out);
void FreeMemory(MemAlloc& a);
uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0);

struct Buf {                     // buffer plus its memory
  VkBuffer buf = VK_NULL_HANDLE;
  MemAlloc mem;
  VkDeviceSize size = 0;
  void* map = nullptr;
};
bool CreateBuf(Buf* b, VkDeviceSize size, VkBufferUsageFlags usage, MemKind kind);
void DestroyBuf(Buf* b);

// Run `fn` once the timeline reaches `value` (or immediately if it already has).
void DeferUntil(uint64_t value, std::function<void()> fn);
void RunDeferred();               // called from submit paths and finish
uint64_t TimelineDone();          // value the GPU has reached
bool WaitTimeline(uint64_t value, uint64_t timeoutNs);   // true when reached
uint64_t NextTimeline();          // reserve the next signal value (monotonic)
uint64_t TimelineLast();          // last value reserved so far (everything submitted is at most this)

}  // namespace nvn
