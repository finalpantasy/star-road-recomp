// Star Road Recomp: NVN on Vulkan, Vulkan bring-up (task-12). Our own code (D-008).
#include "nvn_vk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

#include "nvn_internal.h"

namespace nvn {

Vk g_vk;
static std::string g_err;
const std::string& LastVkError() { return g_err; }

static void SetErr(const char* fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  g_err = b;
  Err("vulkan: %s", b);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
  if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
    std::lock_guard<std::mutex> lk(g_vk.valLock);
    auto& t = g_vk.valById[d->pMessageIdName ? d->pMessageIdName : "(no id)"];
    if (t.count++ == 0) t.first = d->pMessage ? d->pMessage : "";
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) t.error = true;
  }
  if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    if (g_vk.valErrors++ == 0) g_vk.firstValError = d->pMessage ? d->pMessage : "";
    Err("VALIDATION ERROR: %s", d->pMessage ? d->pMessage : "");
  } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    g_vk.valWarnings++;
    Log("validation warning: %s", d->pMessage ? d->pMessage : "");
  }
  return VK_FALSE;
}

#define LOAD_I(n) \
  g_vk.f.n = (PFN_##n)g_vk.f.vkGetInstanceProcAddr(g_vk.inst, #n);
#define LOAD_D(n) \
  g_vk.f.n = (PFN_##n)g_vk.f.vkGetDeviceProcAddr(g_vk.dev, #n);

static bool HasExt(const std::vector<VkExtensionProperties>& v, const char* n) {
  for (auto& e : v)
    if (!strcmp(e.extensionName, n)) return true;
  return false;
}

bool VkBringUp(const VkInitOptions& opt) {
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  if (g_vk.inited) return true;
  g_vk.lib = LoadLibraryA("vulkan-1.dll");
  if (!g_vk.lib) { SetErr("vulkan-1.dll not found"); return false; }
  g_vk.f.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(g_vk.lib, "vkGetInstanceProcAddr");
  if (!g_vk.f.vkGetInstanceProcAddr) { SetErr("vkGetInstanceProcAddr missing"); return false; }
  auto gipa = g_vk.f.vkGetInstanceProcAddr;
  g_vk.f.vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
  g_vk.f.vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
  g_vk.f.vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)gipa(nullptr, "vkEnumerateInstanceLayerProperties");

  if (opt.validation && !opt.layerPath.empty()) SetEnvironmentVariableA("VK_LAYER_PATH", opt.layerPath.c_str());

  uint32_t n = 0;
  g_vk.f.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> iexts(n);
  g_vk.f.vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
  std::vector<const char*> iext, layers;
  if (HasExt(iexts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { iext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); g_vk.hasDebugUtils = true; }
  if (opt.surfaceExtensions) {
    iext.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    iext.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
  }
  if (opt.validation) layers.push_back("VK_LAYER_KHRONOS_validation");
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "StarRoadRecomp";
  ai.pEngineName = "nvn-on-vulkan";
  ai.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai;
  ici.enabledExtensionCount = (uint32_t)iext.size();
  ici.ppEnabledExtensionNames = iext.data();
  ici.enabledLayerCount = (uint32_t)layers.size();
  ici.ppEnabledLayerNames = layers.data();
  VkResult r = g_vk.f.vkCreateInstance(&ici, nullptr, &g_vk.inst);
  if (r != VK_SUCCESS) { SetErr("vkCreateInstance failed (%d)%s", r, opt.validation ? " (validation layer not found?)" : ""); return false; }
  NVN_VK_INSTANCE_FNS(LOAD_I)

  if (opt.validation && g_vk.hasDebugUtils && g_vk.f.vkCreateDebugUtilsMessengerEXT) {
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = DebugCb;
    g_vk.f.vkCreateDebugUtilsMessengerEXT(g_vk.inst, &mi, nullptr, &g_vk.messenger);
  }

  // ---- physical device
  g_vk.f.vkEnumeratePhysicalDevices(g_vk.inst, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n);
  g_vk.f.vkEnumeratePhysicalDevices(g_vk.inst, &n, pds.data());
  int best = -1, bestScore = -1;
  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceProperties p;
    g_vk.f.vkGetPhysicalDeviceProperties(pds[i], &p);
    if (p.apiVersion < VK_API_VERSION_1_3) continue;
    int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 50 : 10;
    if (opt.deviceIndex >= 0) score = (int)i == opt.deviceIndex ? 1000 : 0;
    if (score > bestScore) { bestScore = score; best = (int)i; }
  }
  if (best < 0) { SetErr("no Vulkan 1.3 device"); return false; }
  g_vk.phys = pds[best];
  g_vk.f.vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
  g_vk.f.vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.memProps);
  g_vk.granularity = std::max<VkDeviceSize>(1, g_vk.props.limits.bufferImageGranularity);

  g_vk.f.vkEnumerateDeviceExtensionProperties(g_vk.phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> dexts(n);
  g_vk.f.vkEnumerateDeviceExtensionProperties(g_vk.phys, nullptr, &n, dexts.data());
  std::vector<const char*> dext;
  auto want = [&](const char* name, bool* flag) {
    if (HasExt(dexts, name)) { dext.push_back(name); if (flag) *flag = true; }
  };
  if (opt.surfaceExtensions) want(VK_KHR_SWAPCHAIN_EXTENSION_NAME, &g_vk.hasSwapchain);    // the instance has VK_KHR_surface only then
  want(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, &g_vk.hasHostImport);
  want(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, &g_vk.hasDepthClipControl);
  want(VK_EXT_4444_FORMATS_EXTENSION_NAME, &g_vk.has4444);
  want(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, &g_vk.hasIdx8);
  want(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, &g_vk.hasCustomBorder);

  // features: enable every supported core feature except robustness
  g_vk.f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  g_vk.f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  g_vk.f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  g_vk.depthClip.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
  g_vk.f4444.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT;
  g_vk.idx8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT;
  g_vk.customBorder.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
  g_vk.feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  void** tail = &g_vk.feat.pNext;
  auto chain = [&](void* s) { *tail = s; tail = (void**)&((VkBaseOutStructure*)s)->pNext; *tail = nullptr; };
  chain(&g_vk.f11);
  chain(&g_vk.f12);
  chain(&g_vk.f13);
  if (g_vk.hasDepthClipControl) chain(&g_vk.depthClip);
  if (g_vk.has4444) chain(&g_vk.f4444);
  if (g_vk.hasIdx8) chain(&g_vk.idx8);
  if (g_vk.hasCustomBorder) chain(&g_vk.customBorder);
  g_vk.f.vkGetPhysicalDeviceFeatures2(g_vk.phys, &g_vk.feat);
  g_vk.feat.features.robustBufferAccess = VK_FALSE;
  g_vk.f13.robustImageAccess = VK_FALSE;
  if (!g_vk.f13.dynamicRendering || !g_vk.f13.synchronization2 || !g_vk.f12.timelineSemaphore) {
    SetErr("device lacks dynamicRendering / synchronization2 / timelineSemaphore");
    return false;
  }
  g_vk.hasMinMax = g_vk.f12.samplerFilterMinmax != 0;

  VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
  {
    g_vk.p11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    g_vk.p13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &g_vk.p11; g_vk.p11.pNext = &g_vk.p13; g_vk.p13.pNext = g_vk.hasHostImport ? (void*)&hp : nullptr;
    g_vk.f.vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    g_vk.p11.pNext = nullptr; g_vk.p13.pNext = nullptr;
    if (g_vk.hasHostImport) g_vk.hostAlign = hp.minImportedHostPointerAlignment;
  }

  uint32_t nq = 0;
  g_vk.f.vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  g_vk.f.vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qf.data());
  int fam = -1;
  for (uint32_t i = 0; i < nq; i++)
    if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) { fam = (int)i; break; }
  if (fam < 0) { SetErr("no graphics+compute queue family"); return false; }
  g_vk.family = (uint32_t)fam;
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = g_vk.family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &g_vk.feat;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qi;
  dci.enabledExtensionCount = (uint32_t)dext.size();
  dci.ppEnabledExtensionNames = dext.data();
  r = g_vk.f.vkCreateDevice(g_vk.phys, &dci, nullptr, &g_vk.dev);
  if (r != VK_SUCCESS) { SetErr("vkCreateDevice failed (%d)", r); return false; }
  NVN_VK_DEVICE_FNS(LOAD_D)
  g_vk.f.vkGetDeviceQueue(g_vk.dev, g_vk.family, 0, &g_vk.queue);

  VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  ti.initialValue = 0;
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  si.pNext = &ti;
  g_vk.f.vkCreateSemaphore(g_vk.dev, &si, nullptr, &g_vk.timeline);
  g_vk.inited = true;
  Log("vulkan: %s api %u.%u.%u hostImport=%d depthClipControl=%d 4444=%d swapchain=%d validation=%d", g_vk.props.deviceName,
      VK_VERSION_MAJOR(g_vk.props.apiVersion), VK_VERSION_MINOR(g_vk.props.apiVersion), VK_VERSION_PATCH(g_vk.props.apiVersion),
      g_vk.hasHostImport, g_vk.hasDepthClipControl, g_vk.has4444, g_vk.hasSwapchain, opt.validation);
  return true;
}

// ---- memory sub-allocator ---------------------------------------------------------------------------------------
struct MemBlock {
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  uint32_t type = 0;
  void* mapped = nullptr;
  bool dedicated = false;
  std::map<VkDeviceSize, VkDeviceSize> freeList;   // offset -> size
};
static std::map<uint32_t, std::vector<MemBlock*>> g_blocks;

uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
  for (uint32_t i = 0; i < g_vk.memProps.memoryTypeCount; i++) {
    if (!(bits & (1u << i))) continue;
    VkMemoryPropertyFlags f = g_vk.memProps.memoryTypes[i].propertyFlags;
    if ((f & want) == want && !(f & avoid)) return i;
  }
  return UINT32_MAX;
}

static VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

bool AllocMemory(MemKind kind, const VkMemoryRequirements& req, MemAlloc* out) {
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  uint32_t type;
  if (kind == MemKind::Device) {
    type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == UINT32_MAX) type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  } else {
    type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  if (type == UINT32_MAX) return false;
  VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, g_vk.granularity);
  VkDeviceSize blockSize = kind == MemKind::Device ? (256ull << 20) : (64ull << 20);
  bool host = kind == MemKind::HostVisible;
  auto& vec = g_blocks[type];
  if (req.size <= blockSize / 4) {
    for (MemBlock* b : vec) {
      if (b->dedicated) continue;
      for (auto it = b->freeList.begin(); it != b->freeList.end(); ++it) {
        VkDeviceSize off = AlignUp(it->first, align);
        if (off + req.size <= it->first + it->second) {
          VkDeviceSize fo = it->first, fs = it->second;
          b->freeList.erase(it);
          if (off > fo) b->freeList[fo] = off - fo;
          if (off + req.size < fo + fs) b->freeList[off + req.size] = fo + fs - (off + req.size);
          out->mem = b->mem; out->offset = off; out->size = req.size; out->block = b;
          out->mapped = host ? (char*)b->mapped + off : nullptr;
          return true;
        }
      }
    }
  }
  MemBlock* b = new MemBlock;
  bool ded = req.size > blockSize / 4;
  b->size = ded ? AlignUp(req.size, align) : blockSize;
  b->type = type;
  b->dedicated = ded;
  VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mi.allocationSize = b->size;
  mi.memoryTypeIndex = type;
  if (g_vk.f.vkAllocateMemory(g_vk.dev, &mi, nullptr, &b->mem) != VK_SUCCESS) { delete b; return false; }
  if (host && g_vk.f.vkMapMemory(g_vk.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS) { g_vk.f.vkFreeMemory(g_vk.dev, b->mem, nullptr); delete b; return false; }
  if (!ded) b->freeList[req.size] = b->size - req.size;
  vec.push_back(b);
  out->mem = b->mem; out->offset = 0; out->size = req.size; out->block = b;
  out->mapped = host ? b->mapped : nullptr;
  return true;
}

void FreeMemory(MemAlloc& a) {
  if (!a.block) return;
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  MemBlock* b = a.block;
  if (b->dedicated) {
    auto& v = g_blocks[b->type];
    v.erase(std::remove(v.begin(), v.end(), b), v.end());
    g_vk.f.vkFreeMemory(g_vk.dev, b->mem, nullptr);
    delete b;
  } else {
    VkDeviceSize off = a.offset, sz = a.size;
    auto nx = b->freeList.lower_bound(off);
    if (nx != b->freeList.begin()) {
      auto pv = std::prev(nx);
      if (pv->first + pv->second == off) { off = pv->first; sz += pv->second; b->freeList.erase(pv); }
    }
    if (nx != b->freeList.end() && off + sz == nx->first) { sz += nx->second; b->freeList.erase(nx); }
    b->freeList[off] = sz;
  }
  a = MemAlloc{};
}

bool CreateBuf(Buf* b, VkDeviceSize size, VkBufferUsageFlags usage, MemKind kind) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (g_vk.f.vkCreateBuffer(g_vk.dev, &bi, nullptr, &b->buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  g_vk.f.vkGetBufferMemoryRequirements(g_vk.dev, b->buf, &req);
  if (!AllocMemory(kind, req, &b->mem)) { g_vk.f.vkDestroyBuffer(g_vk.dev, b->buf, nullptr); b->buf = VK_NULL_HANDLE; return false; }
  g_vk.f.vkBindBufferMemory(g_vk.dev, b->buf, b->mem.mem, b->mem.offset);
  b->size = size;
  b->map = b->mem.mapped;
  return true;
}

void DestroyBuf(Buf* b) {
  if (b->buf) g_vk.f.vkDestroyBuffer(g_vk.dev, b->buf, nullptr);
  FreeMemory(b->mem);
  *b = Buf{};
}

// ---- timeline and deferred work ---------------------------------------------------------------------------------
static std::atomic<uint64_t> g_tlNext{0};
static std::deque<std::pair<uint64_t, std::function<void()>>> g_deferred;

uint64_t NextTimeline() { return ++g_tlNext; }
uint64_t TimelineLast() { return g_tlNext; }

uint64_t TimelineDone() {
  uint64_t v = 0;
  if (g_vk.timeline) g_vk.f.vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
  return v;
}

bool WaitTimeline(uint64_t value, uint64_t timeoutNs) {
  if (value == 0 || TimelineDone() >= value) return true;
  VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wi.semaphoreCount = 1;
  wi.pSemaphores = &g_vk.timeline;
  wi.pValues = &value;
  return g_vk.f.vkWaitSemaphores(g_vk.dev, &wi, timeoutNs) == VK_SUCCESS;
}

void DeferUntil(uint64_t value, std::function<void()> fn) {
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  if (value <= TimelineDone()) { fn(); return; }
  g_deferred.emplace_back(value, std::move(fn));
}

void RunDeferred() {
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  uint64_t done = TimelineDone();
  // deferred entries are queued in nondecreasing order of their values per producer, but producers interleave: scan all
  for (size_t i = 0; i < g_deferred.size();) {
    if (g_deferred[i].first <= done) {
      auto fn = std::move(g_deferred[i].second);
      g_deferred.erase(g_deferred.begin() + (ptrdiff_t)i);
      fn();
    } else i++;
  }
}

void VkTearDown() {
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  if (!g_vk.inited) return;
  g_vk.f.vkDeviceWaitIdle(g_vk.dev);
  RunDeferred();
  for (auto& kv : g_blocks)
    for (MemBlock* b : kv.second) { g_vk.f.vkFreeMemory(g_vk.dev, b->mem, nullptr); delete b; }
  g_blocks.clear();
  g_vk.f.vkDestroySemaphore(g_vk.dev, g_vk.timeline, nullptr);
  g_vk.f.vkDestroyDevice(g_vk.dev, nullptr);
  if (g_vk.messenger) g_vk.f.vkDestroyDebugUtilsMessengerEXT(g_vk.inst, g_vk.messenger, nullptr);
  g_vk.f.vkDestroyInstance(g_vk.inst, nullptr);
  g_vk.inited = false;
  g_vk.dev = VK_NULL_HANDLE;
  g_vk.inst = VK_NULL_HANDLE;
  g_vk.timeline = VK_NULL_HANDLE;
  g_tlNext = 0;
}

}  // namespace nvn
