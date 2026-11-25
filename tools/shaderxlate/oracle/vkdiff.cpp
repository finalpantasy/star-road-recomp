// vkdiff: headless Vulkan shader differential harness (lavapipe only).
// Renders shader variant A and variant B with identical inputs and compares
// the output attachments bit by bit. See README.md.
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// ---------------------------------------------------------------- loader
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;

#define GLOBAL_FNS(X) X(CreateInstance) X(EnumerateInstanceLayerProperties)
#define INST_FNS(X) \
  X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
  X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceMemoryProperties) \
  X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(GetDeviceProcAddr) \
  X(EnumerateDeviceExtensionProperties)
#define DEV_FNS(X) \
  X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) \
  X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
  X(MapMemory) X(UnmapMemory) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) \
  X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
  X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) \
  X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
  X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
  X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) \
  X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
  X(CmdPipelineBarrier) X(CmdClearAttachments) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdBeginRendering) \
  X(CmdEndRendering) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) \
  X(CmdDraw) X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit) \
  X(DeviceWaitIdle)

#define DECL(n) static PFN_vk##n vk##n;
GLOBAL_FNS(DECL) INST_FNS(DECL) DEV_FNS(DECL)
#undef DECL
// transform feedback (geometry-shader jobs): loaded only when the device has VK_EXT_transform_feedback
static PFN_vkCmdBindTransformFeedbackBuffersEXT vkCmdBindTransformFeedbackBuffersEXT;
static PFN_vkCmdBeginTransformFeedbackEXT vkCmdBeginTransformFeedbackEXT;
static PFN_vkCmdEndTransformFeedbackEXT vkCmdEndTransformFeedbackEXT;
static bool g_xfb = false;
static PFN_vkCreateDebugUtilsMessengerEXT pfnCreateMessenger;

// ---------------------------------------------------------------- utils
struct Fail { std::string msg; };

static const char* resName(VkResult r) {
  switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "MEMORY_MAP_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
    case VK_ERROR_UNKNOWN: return "UNKNOWN";
    case VK_ERROR_INVALID_SHADER_NV: return "INVALID_SHADER";
    default: return "other";
  }
}
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) \
  throw Fail{std::string(#x) + " failed: " + resName(r_) + " (" + std::to_string((int)r_) + ")"}; } while (0)

static uint32_t fmix(uint32_t h) {
  h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16; return h;
}
static uint32_t H(uint32_t seed, uint32_t slot, uint32_t i) {
  return fmix(seed * 0x9E3779B1u ^ slot * 0x85EBCA77u ^ i);
}
static uint32_t fnv(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) { h ^= c; h *= 16777619u; }
  return h;
}
static uint32_t F2U(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float U2F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

struct Scope {  // runs registered cleanups in reverse order
  std::vector<std::function<void()>> fns;
  void add(std::function<void()> f) { fns.push_back(std::move(f)); }
  ~Scope() { for (size_t i = fns.size(); i-- > 0;) fns[i](); }
};

static std::string sanitize(std::string s) {
  for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

// ---------------------------------------------------------------- globals
static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_q;
static VkCommandPool g_pool;
static VkCommandBuffer g_cb;
static VkFence g_fence;
static VkPhysicalDeviceMemoryProperties g_mem;
static uint32_t g_maxUbo = 65536;
static bool g_validate = false;
static std::string g_case;
static int g_valErrors = 0;
static FILE* g_results;
static std::string g_dumpDir;
static bool g_time = false;

struct TimeoutEx {};

static void writeResult(const std::string& id, const std::string& status, uint64_t values, uint64_t ex,
                        uint64_t cl, uint64_t bad, double maxd, const std::string& first,
                        const std::string& msg) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.9g", maxd);
  fprintf(g_results, "%s\t%s\t%llu\t%llu\t%llu\t%llu\t%s\t%s\t%s\n", id.c_str(), status.c_str(),
          (unsigned long long)values, (unsigned long long)ex, (unsigned long long)cl,
          (unsigned long long)bad, buf, first.c_str(), sanitize(msg).c_str());
  fflush(g_results);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL dbgCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
  if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    g_valErrors++;
    fprintf(stderr, "[validation error] case %s: %s\n", g_case.c_str(), d->pMessage);
  } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    fprintf(stderr, "[validation warning] case %s: %s\n", g_case.c_str(), d->pMessage);
  }
  return VK_FALSE;
}

// ---------------------------------------------------------------- memory
static uint32_t findMem(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

struct Buf { VkBuffer b = 0; VkDeviceMemory m = 0; void* p = nullptr; size_t size = 0; };

static void freeBuf(Buf& b) {
  if (b.m && b.p) vkUnmapMemory(g_dev, b.m);
  if (b.b) vkDestroyBuffer(g_dev, b.b, nullptr);
  if (b.m) vkFreeMemory(g_dev, b.m, nullptr);
  b = Buf{};
}

static Buf makeBuf(size_t size, VkBufferUsageFlags usage) {
  Buf r; r.size = size;
  try {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(g_dev, &ci, nullptr, &r.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, r.b, &mr);
    uint32_t t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX) throw Fail{"no host visible memory type"};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = t;
    VKC(vkAllocateMemory(g_dev, &ai, nullptr, &r.m));
    VKC(vkBindBufferMemory(g_dev, r.b, r.m, 0));
    VKC(vkMapMemory(g_dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
  } catch (...) { freeBuf(r); throw; }
  return r;
}
static VkDeviceMemory imageMem(VkImage img) {
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, img, &mr);
  uint32_t t = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (t == UINT32_MAX) t = findMem(mr.memoryTypeBits, 0);
  if (t == UINT32_MAX) throw Fail{"no image memory type"};
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = t;
  VkDeviceMemory m; VKC(vkAllocateMemory(g_dev, &ai, nullptr, &m));
  VkResult r = vkBindImageMemory(g_dev, img, m, 0);
  if (r != VK_SUCCESS) { vkFreeMemory(g_dev, m, nullptr); throw Fail{"vkBindImageMemory failed"}; }
  return m;
}

// submit g_cb and wait; on timeout the caller records the result and terminates with 3
static void submitWait() {
  VKC(vkResetFences(g_dev, 1, &g_fence));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1; si.pCommandBuffers = &g_cb;
  VKC(vkQueueSubmit(g_q, 1, &si, g_fence));
  VkResult r = vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 20ull * 1000000000ull);
  if (r == VK_TIMEOUT) throw TimeoutEx{};
  VKC(r);
}

// ---------------------------------------------------------------- layout
struct Res {
  char kind = 'u';  // 'u' ubo, 'f' ubo from file, 's' support, 't' texture, 'b' storage buffer
  int set = 0, binding = 0, slot = 0;
  std::string dim, key; int shadow = 0;
};
struct Layout {
  std::vector<Res> res;
  std::vector<int> attrs, rts;
  int fbw = 0, fbh = 0;
  int xfbStride = 0, prims = 0, maxVerts = 0;  // gs jobs: capture record size, triangles drawn, GS max_vertices
};

static std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> o; size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    size_t j = i;
    while (j < s.size() && !isspace((unsigned char)s[j])) j++;
    if (j > i) o.push_back(s.substr(i, j - i));
    i = j;
  }
  return o;
}

static std::vector<uint8_t> readFile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) throw Fail{"cannot open " + path};
  std::vector<uint8_t> d; static uint8_t tmp[65536]; size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) d.insert(d.end(), tmp, tmp + n);
  fclose(f);
  return d;
}

static Layout parseLayout(const std::string& path) {
  auto d = readFile(path);
  std::string text(d.begin(), d.end());
  Layout L; size_t pos = 0;
  while (pos <= text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    auto t = splitWs(line);
    if (t.empty()) continue;
    try {
      if (t[0] == "ubo" && t.size() >= 4) {
        Res r; r.kind = 'u'; r.set = std::stoi(t[1]); r.binding = std::stoi(t[2]); r.slot = std::stoi(t[3]);
        L.res.push_back(r);
      } else if (t[0] == "ubofile" && t.size() >= 4) {
        Res r; r.kind = 'f'; r.set = std::stoi(t[1]); r.binding = std::stoi(t[2]); r.key = t[3];
        L.res.push_back(r);
      } else if (t[0] == "ssbo" && t.size() >= 4) {
        // ssbo <set> <binding> <nvn storage descriptor index>: same seeded bytes for both sides
        Res r; r.kind = 'b'; r.set = std::stoi(t[1]); r.binding = std::stoi(t[2]); r.slot = std::stoi(t[3]);
        L.res.push_back(r);
      } else if (t[0] == "support" && t.size() >= 3) {
        Res r; r.kind = 's'; r.set = std::stoi(t[1]); r.binding = std::stoi(t[2]); L.res.push_back(r);
      } else if (t[0] == "tex" && t.size() >= 6) {
        Res r; r.kind = 't'; r.set = std::stoi(t[1]); r.binding = std::stoi(t[2]); r.dim = t[3];
        r.shadow = std::stoi(t[4]); r.key = t[5]; L.res.push_back(r);
      } else if (t[0] == "attr" && t.size() >= 2) {
        L.attrs.push_back(std::stoi(t[1]));
      } else if (t[0] == "rt" && t.size() >= 2) {
        L.rts.push_back(std::stoi(t[1]));
      } else if (t[0] == "fb" && t.size() >= 3) {
        L.fbw = std::stoi(t[1]); L.fbh = std::stoi(t[2]);
      } else if (t[0] == "xfb" && t.size() >= 4) {
        L.xfbStride = std::stoi(t[1]); L.prims = std::stoi(t[2]); L.maxVerts = std::stoi(t[3]);
      } else {
        fprintf(stderr, "warning: %s: ignoring layout line '%s'\n", path.c_str(), t[0].c_str());
      }
    } catch (...) { throw Fail{"bad layout line in " + path + ": " + t[0]}; }
  }
  return L;
}

// ---------------------------------------------------------------- caches
struct Tex { VkImage img = 0; VkDeviceMemory mem = 0; VkImageView view = 0; VkSampler samp = 0; bool shadow = false; };
static std::map<std::pair<uint32_t, int>, Buf> g_ubo;
static std::map<std::pair<uint32_t, int>, Buf> g_vtx;
static std::map<std::pair<uint32_t, int>, Buf> g_ssbo;
static std::map<std::pair<int, int>, Buf> g_support;
static std::map<std::string, Buf> g_ubof;
static std::map<std::string, Tex> g_tex;
static std::map<std::string, VkShaderModule> g_mod;

struct Targets {
  std::vector<VkImage> img; std::vector<VkImageView> view; std::vector<VkDeviceMemory> mem;
  std::vector<Buf> rb;
};
static std::map<std::tuple<int, int, int>, Targets> g_targets;

static void clearInputCaches() {
  vkDeviceWaitIdle(g_dev);
  for (auto& kv : g_ubo) freeBuf(kv.second);
  for (auto& kv : g_vtx) freeBuf(kv.second);
  for (auto& kv : g_ubof) freeBuf(kv.second);
  for (auto& kv : g_ssbo) freeBuf(kv.second);
  g_ubo.clear(); g_vtx.clear(); g_ubof.clear(); g_ssbo.clear();
}

static VkBuffer uboBuf(uint32_t seed, int slot) {
  if (g_ubo.size() > 128) clearInputCaches();
  auto k = std::make_pair(seed, slot);
  auto it = g_ubo.find(k);
  if (it != g_ubo.end()) return it->second.b;
  Buf b = makeBuf(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  uint32_t* w = (uint32_t*)b.p;
  for (uint32_t i = 0; i < 16384; i++) w[i] = F2U((float)(H(seed, (uint32_t)slot, i) % 4096u) / 4096.0f);
  // c[0] storage descriptor addresses (0x110 + 16k): 256-byte aligned, as a driver would place them
  if (slot == 0)
    for (uint32_t d = 0; d < 16; d++) w[(0x110 + 16 * d) / 4] &= ~0xFFu;
  g_ubo[k] = b;
  return b.b;
}
// ubofile: exact bytes (for example a program's own c[1] constants), zero padded to 64 KiB
static VkBuffer uboFileBuf(const std::string& path) {
  if (g_ubof.size() > 128) clearInputCaches();
  auto it = g_ubof.find(path);
  if (it != g_ubof.end()) return it->second.b;
  auto d = readFile(path);
  Buf b = makeBuf(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  memset(b.p, 0, 65536);
  memcpy(b.p, d.data(), std::min<size_t>(d.size(), 65536));
  g_ubof[path] = b;
  return b.b;
}
static const uint32_t kSsboBytes = 1u << 20;
static VkBuffer ssboBuf(uint32_t seed, int index) {
  if (g_ssbo.size() > 64) clearInputCaches();
  auto k = std::make_pair(seed, index);
  auto it = g_ssbo.find(k);
  if (it != g_ssbo.end()) return it->second.b;
  Buf b = makeBuf(kSsboBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  uint32_t* w = (uint32_t*)b.p;
  for (uint32_t i = 0; i < kSsboBytes / 4; i++)
    w[i] = F2U((float)(H(seed, 1000u + (uint32_t)index, i) % 4096u) / 4096.0f);
  g_ssbo[k] = b;
  return b.b;
}
static VkBuffer supportBuf(int fbw, int fbh) {
  auto k = std::make_pair(fbw, fbh);
  auto it = g_support.find(k);
  if (it != g_support.end()) return it->second.b;
  Buf b = makeBuf(1600, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  memset(b.p, 0, 1600);
  float* f = (float*)b.p;
  for (int i = 0; i <= 72; i++) f[(192 + 16 * i) / 4] = 1.0f;
  float a[4] = {(float)fbw, (float)fbh, 1.f, 1.f};
  memcpy(f + 160 / 4, a, 16);
  float c[4] = {2.f / fbw, 2.f / fbh, 1.f, 1.f};
  memcpy(f + 144 / 4, c, 16);
  g_support[k] = b;
  return b.b;
}
static VkBuffer vtxBuf(uint32_t seed, int loc) {
  auto k = std::make_pair(seed, loc);
  auto it = g_vtx.find(k);
  if (it != g_vtx.end()) return it->second.b;
  Buf b = makeBuf(64 * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  float* f = (float*)b.p;
  for (uint32_t v = 0; v < 64; v++)
    for (uint32_t c = 0; c < 4; c++)
      f[v * 4 + c] = (float)(H(seed, 1000u + (uint32_t)loc, 4 * v + c) % 4096u) / 2048.0f - 1.0f;
  g_vtx[k] = b;
  return b.b;
}

static VkShaderModule shaderMod(const std::string& path) {
  auto it = g_mod.find(path);
  if (it != g_mod.end()) return it->second;
  auto d = readFile(path);
  if (d.size() < 20 || d.size() % 4) throw Fail{"bad SPIR-V size: " + path};
  std::vector<uint32_t> w(d.size() / 4);
  memcpy(w.data(), d.data(), d.size());
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = d.size(); ci.pCode = w.data();
  VkShaderModule m; VKC(vkCreateShaderModule(g_dev, &ci, nullptr, &m));
  g_mod[path] = m;
  return m;
}

struct DimInfo { VkImageType type; VkImageViewType view; int size; int layers; bool cube; bool is3d; };
static DimInfo dimInfo(const std::string& d) {
  if (d == "1d") return {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 16, 1, false, false};
  if (d == "2d") return {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 16, 1, false, false};
  if (d == "2d_array") return {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 16, 4, false, false};
  if (d == "3d") return {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 8, 1, false, true};
  if (d == "cube") return {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 16, 6, true, false};
  if (d == "cube_array") return {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 16, 12, true, false};
  throw Fail{"unknown texture dim " + d};
}

// 2dms: 16x16 RGBA8 with 4 samples. Copies into multisampled images are not allowed, so the
// texels are written with clear rects inside a render pass: every 2x2 cell gets its own seeded
// color (all samples of a texel share it; the sample index is not exercised).
static Tex makeMsTex(const std::string& key) {
  const uint32_t N = 16;
  Tex t;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {N, N, 1};
  ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_4_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(g_dev, &ci, nullptr, &t.img));
  t.mem = imageMem(t.img);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = t.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VKC(vkCreateImageView(g_dev, &vi, nullptr, &t.view));
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.maxAnisotropy = 1.f;
  VKC(vkCreateSampler(g_dev, &si, nullptr, &t.samp));
  VKC(vkResetCommandBuffer(g_cb, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(g_cb, &bi));
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.image = t.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = t.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {N, N}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  vkCmdBeginRendering(g_cb, &ri);
  uint32_t kh = fnv(key);
  for (uint32_t y = 0; y < N; y += 2)
    for (uint32_t x = 0; x < N; x += 2) {
      uint32_t u = fmix(fmix(kh ^ (x * 0xC2B2AE3Du)) ^ (y * 0x27D4EB2Fu));
      VkClearAttachment cl{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
      for (int c = 0; c < 4; c++) cl.clearValue.color.float32[c] = (float)((u >> (8 * c)) & 0xFF) / 255.0f;
      VkClearRect rc{{{(int32_t)x, (int32_t)y}, {2, 2}}, 0, 1};
      vkCmdClearAttachments(g_cb, 1, &cl, 1, &rc);
    }
  vkCmdEndRendering(g_cb);
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
  VKC(vkEndCommandBuffer(g_cb));
  submitWait();
  return t;
}

static const Tex& getTex(const Res& r) {
  std::string ck = r.key + "|" + r.dim + "|" + std::to_string(r.shadow);
  auto it = g_tex.find(ck);
  if (it != g_tex.end()) return it->second;
  if (r.dim == "2dms") return g_tex[ck] = makeMsTex(r.key);
  DimInfo di = dimInfo(r.dim);
  bool sh = r.shadow != 0;
  VkFormat fmt = sh ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
  int mips = 1; for (int s = di.size; s > 1; s >>= 1) mips++;
  Tex t; t.shadow = sh;
  Buf stage;
  try {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = di.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = di.type; ci.format = fmt;
    ci.extent = {(uint32_t)di.size, di.type == VK_IMAGE_TYPE_1D ? 1u : (uint32_t)di.size,
                 di.is3d ? (uint32_t)di.size : 1u};
    ci.mipLevels = mips; ci.arrayLayers = di.layers; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(g_dev, &ci, nullptr, &t.img));
    t.mem = imageMem(t.img);
    VkImageAspectFlags asp = sh ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.img; vi.viewType = di.view; vi.format = fmt;
    vi.subresourceRange = {asp, 0, (uint32_t)mips, 0, (uint32_t)di.layers};
    VKC(vkCreateImageView(g_dev, &vi, nullptr, &t.view));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 16.f; si.anisotropyEnable = VK_FALSE; si.maxAnisotropy = 1.f;
    si.compareEnable = sh ? VK_TRUE : VK_FALSE; si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VKC(vkCreateSampler(g_dev, &si, nullptr, &t.samp));

    // staging data (RGBA8 and D32 are both 4 bytes per texel)
    uint32_t kh = fnv(r.key);
    size_t total = 0;
    for (int m = 0; m < mips; m++) {
      size_t w = std::max(1, di.size >> m), h = di.type == VK_IMAGE_TYPE_1D ? 1 : w, dd = di.is3d ? w : 1;
      total += w * h * dd * 4 * di.layers;
    }
    stage = makeBuf(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::vector<VkBufferImageCopy> regions;
    uint32_t* dst = (uint32_t*)stage.p; size_t off = 0;
    for (int m = 0; m < mips; m++) {
      uint32_t w = std::max(1, di.size >> m), h = di.type == VK_IMAGE_TYPE_1D ? 1u : w, dd = di.is3d ? w : 1u;
      for (int l = 0; l < di.layers; l++) {
        VkBufferImageCopy rg{};
        rg.bufferOffset = off;
        rg.imageSubresource = {asp, (uint32_t)m, (uint32_t)l, 1};
        rg.imageExtent = {w, h, dd};
        regions.push_back(rg);
        for (uint32_t z = 0; z < dd; z++) for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
          uint32_t u = fmix(kh ^ ((uint32_t)m * 0x9E3779B1u));
          u = fmix(u ^ ((uint32_t)l * 0x85EBCA77u)); u = fmix(u ^ (x * 0xC2B2AE3Du));
          u = fmix(u ^ (y * 0x27D4EB2Fu)); u = fmix(u ^ (z * 0x165667B1u));
          dst[off / 4] = sh ? F2U((float)(u % 4096u) / 4096.0f) : u;
          off += 4;
        }
      }
    }
    VKC(vkResetCommandBuffer(g_cb, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(g_cb, &bi));
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = t.img; b.subresourceRange = {asp, 0, (uint32_t)mips, 0, (uint32_t)di.layers};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdCopyBufferToImage(g_cb, stage.b, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = sh ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VKC(vkEndCommandBuffer(g_cb));
    submitWait();
  } catch (...) {
    freeBuf(stage);
    if (t.samp) vkDestroySampler(g_dev, t.samp, nullptr);
    if (t.view) vkDestroyImageView(g_dev, t.view, nullptr);
    if (t.img) vkDestroyImage(g_dev, t.img, nullptr);
    if (t.mem) vkFreeMemory(g_dev, t.mem, nullptr);
    throw;
  }
  freeBuf(stage);
  return g_tex[ck] = t;
}

static Targets& getTargets(int w, int h, int n) {
  auto k = std::make_tuple(w, h, n);
  auto it = g_targets.find(k);
  if (it != g_targets.end()) return it->second;
  Targets& R = g_targets[k];  // register early; partial state stays owned by the map
  try {
    for (int i = 0; i < n; i++) {
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
      ci.extent = {(uint32_t)w, (uint32_t)h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImage im; VKC(vkCreateImage(g_dev, &ci, nullptr, &im)); R.img.push_back(im);
      R.mem.push_back(imageMem(im));
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView v; VKC(vkCreateImageView(g_dev, &vi, nullptr, &v)); R.view.push_back(v);
      R.rb.push_back(makeBuf((size_t)w * h * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    }
  } catch (...) {
    for (auto v : R.view) vkDestroyImageView(g_dev, v, nullptr);
    for (auto i : R.img) vkDestroyImage(g_dev, i, nullptr);
    for (auto m : R.mem) vkFreeMemory(g_dev, m, nullptr);
    for (auto& b : R.rb) freeBuf(b);
    g_targets.erase(k);
    throw;
  }
  return R;
}

// ---------------------------------------------------------------- render
static const uint32_t SENTINEL = 0x7FC0DEADu;

struct Job {
  std::string id, mode, hvs, hgs, hfs, aspv, alay, bspv, blay;
  uint32_t seed = 0;
};

static std::vector<std::vector<uint32_t>> renderSide(const Job& job, const Layout& L, const std::string& spv, int fbw, int fbh) {
  bool fsMode = job.mode == "fs";
  bool gsMode = job.mode == "gs";
  Scope sc;
  int maxSet = -1;
  for (auto& r : L.res) maxSet = std::max(maxSet, r.set);
  int nsets = maxSet + 1;
  std::vector<VkDescriptorSetLayout> dsl(nsets, VK_NULL_HANDLE);
  std::vector<std::vector<VkDescriptorSetLayoutBinding>> binds(nsets);
  std::vector<std::vector<const Res*>> bres(nsets);
  uint32_t nUbo = 0, nTex = 0, nSsbo = 0;
  for (auto& r : L.res) {
    if (r.set < 0 || r.set > 16) throw Fail{"descriptor set number out of range"};
    bool dup = false;
    for (auto& b : binds[r.set]) if ((int)b.binding == r.binding) dup = true;
    if (dup) { fprintf(stderr, "warning: duplicate binding %d.%d ignored\n", r.set, r.binding); continue; }
    VkDescriptorSetLayoutBinding b{};
    b.binding = r.binding; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    b.descriptorType = r.kind == 't' ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                     : r.kind == 'b' ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    (r.kind == 't' ? nTex : r.kind == 'b' ? nSsbo : nUbo)++;
    binds[r.set].push_back(b); bres[r.set].push_back(&r);
  }
  for (int s = 0; s < nsets; s++) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)binds[s].size(); ci.pBindings = binds[s].data();
    VKC(vkCreateDescriptorSetLayout(g_dev, &ci, nullptr, &dsl[s]));
    VkDescriptorSetLayout h = dsl[s];
    sc.add([h] { vkDestroyDescriptorSetLayout(g_dev, h, nullptr); });
  }
  VkPipelineLayout pl;
  {
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = nsets; ci.pSetLayouts = dsl.data();
    VKC(vkCreatePipelineLayout(g_dev, &ci, nullptr, &pl));
    sc.add([pl] { vkDestroyPipelineLayout(g_dev, pl, nullptr); });
  }
  std::vector<VkDescriptorSet> sets(nsets, VK_NULL_HANDLE);
  if (nsets > 0) {
    std::vector<VkDescriptorPoolSize> ps;
    if (nUbo) ps.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nUbo});
    if (nTex) ps.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nTex});
    if (nSsbo) ps.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nSsbo});
    if (ps.empty()) ps.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = nsets; pi.poolSizeCount = (uint32_t)ps.size(); pi.pPoolSizes = ps.data();
    VkDescriptorPool pool; VKC(vkCreateDescriptorPool(g_dev, &pi, nullptr, &pool));
    sc.add([pool] { vkDestroyDescriptorPool(g_dev, pool, nullptr); });
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = nsets; ai.pSetLayouts = dsl.data();
    VKC(vkAllocateDescriptorSets(g_dev, &ai, sets.data()));
    std::deque<VkDescriptorBufferInfo> bi; std::deque<VkDescriptorImageInfo> ii;
    std::vector<VkWriteDescriptorSet> ws;
    for (int s = 0; s < nsets; s++)
      for (size_t k = 0; k < bres[s].size(); k++) {
        const Res& r = *bres[s][k];
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[s]; w.dstBinding = r.binding; w.descriptorCount = 1;
        if (r.kind == 't') {
          const Tex& t = getTex(r);
          ii.push_back({t.samp, t.view, t.shadow ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
          w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii.back();
        } else if (r.kind == 'b') {
          bi.push_back({ssboBuf(job.seed, r.slot), 0, kSsboBytes});
          w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi.back();
        } else {
          VkDeviceSize range; VkBuffer b;
          if (r.kind == 'u') { b = uboBuf(job.seed, r.slot); range = std::min<uint32_t>(65536, g_maxUbo); }
          else if (r.kind == 'f') { b = uboFileBuf(r.key); range = std::min<uint32_t>(65536, g_maxUbo); }
          else { b = supportBuf(fbw, fbh); range = 1600; }
          bi.push_back({b, 0, range});
          w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi.back();
        }
        ws.push_back(w);
      }
    if (!ws.empty()) vkUpdateDescriptorSets(g_dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
  }
  // attachments
  int nAtt = 1;
  if (fsMode) {
    int mx = 0; for (int r : L.rts) mx = std::max(mx, r);
    nAtt = mx + 1;
    if (nAtt > 8 || nAtt < 1) throw Fail{"bad render target count"};
  }
  std::vector<bool> written(nAtt, !fsMode || L.rts.empty());
  for (int r : L.rts) if (r >= 0 && r < nAtt) written[r] = true;
  Targets& T = getTargets(fbw, fbh, nAtt);

  VkPipeline pipe;
  {
    std::vector<VkPipelineShaderStageCreateInfo> st;
    auto addStage = [&](VkShaderStageFlagBits s, const std::string& p) {
      VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      si.stage = s; si.module = shaderMod(p); si.pName = "main"; st.push_back(si);
    };
    if (fsMode) { addStage(VK_SHADER_STAGE_VERTEX_BIT, job.hvs); addStage(VK_SHADER_STAGE_FRAGMENT_BIT, spv); }
    else if (gsMode) { addStage(VK_SHADER_STAGE_VERTEX_BIT, job.hvs); addStage(VK_SHADER_STAGE_GEOMETRY_BIT, spv); }
    else { addStage(VK_SHADER_STAGE_VERTEX_BIT, spv); addStage(VK_SHADER_STAGE_GEOMETRY_BIT, job.hgs);
           addStage(VK_SHADER_STAGE_FRAGMENT_BIT, job.hfs); }
    std::vector<VkVertexInputBindingDescription> vb; std::vector<VkVertexInputAttributeDescription> va;
    if (!fsMode) for (size_t i = 0; i < L.attrs.size(); i++) {
      vb.push_back({(uint32_t)i, 16, VK_VERTEX_INPUT_RATE_VERTEX});
      va.push_back({(uint32_t)L.attrs[i], (uint32_t)i, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    }
    VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vis.vertexBindingDescriptionCount = (uint32_t)vb.size(); vis.pVertexBindingDescriptions = vb.data();
    vis.vertexAttributeDescriptionCount = (uint32_t)va.size(); vis.pVertexAttributeDescriptions = va.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = (fsMode || gsMode) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    VkViewport vp{0, 0, (float)fbw, (float)fbh, 0, 1};
    VkRect2D sci{{0, 0}, {(uint32_t)fbw, (uint32_t)fbh}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sci;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
    rs.rasterizerDiscardEnable = gsMode ? VK_TRUE : VK_FALSE;   // gs: outputs captured by transform feedback
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> cba(nAtt);
    for (int i = 0; i < nAtt; i++) {
      cba[i] = {}; cba[i].blendEnable = VK_FALSE;
      cba[i].colorWriteMask = written[i] ? 0xF : 0;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = nAtt; cb.pAttachments = cba.data();
    std::vector<VkFormat> fm(nAtt, VK_FORMAT_R32G32B32A32_SFLOAT);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = nAtt; ri.pColorAttachmentFormats = fm.data();
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = (uint32_t)st.size(); gi.pStages = st.data();
    gi.pVertexInputState = &vis; gi.pInputAssemblyState = &ia; gi.pViewportState = &vps;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb;
    gi.layout = pl;
    VKC(vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe));
    sc.add([pipe] { vkDestroyPipeline(g_dev, pipe, nullptr); });
  }

  VKC(vkResetCommandBuffer(g_cb, 0));
  VkCommandBufferBeginInfo bgi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(g_cb, &bgi));
  std::vector<VkImageMemoryBarrier> bars(nAtt);
  for (int i = 0; i < nAtt; i++) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = T.img[i]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    bars[i] = b;
  }
  vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       0, 0, nullptr, 0, nullptr, nAtt, bars.data());
  std::vector<VkRenderingAttachmentInfo> ra(nAtt);
  for (int i = 0; i < nAtt; i++) {
    ra[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ra[i].imageView = T.view[i]; ra[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ra[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ra[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (int c = 0; c < 4; c++) ra[i].clearValue.color.uint32[c] = SENTINEL;  // raw bits via the union
  }
  VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rinfo.renderArea = {{0, 0}, {(uint32_t)fbw, (uint32_t)fbh}}; rinfo.layerCount = 1;
  rinfo.colorAttachmentCount = nAtt; rinfo.pColorAttachments = ra.data();
  vkCmdBeginRendering(g_cb, &rinfo);
  vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  if (nsets > 0)
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, nsets, sets.data(), 0, nullptr);
  if (!fsMode && !L.attrs.empty()) {
    std::vector<VkBuffer> vbs; std::vector<VkDeviceSize> offs(L.attrs.size(), 0);
    for (int a : L.attrs) vbs.push_back(vtxBuf(job.seed, a));
    vkCmdBindVertexBuffers(g_cb, 0, (uint32_t)vbs.size(), vbs.data(), offs.data());
  }
  Buf xb;
  size_t xwords = 0;
  if (gsMode) {
    if (!g_xfb) throw Fail{"VK_EXT_transform_feedback not available"};
    if (L.xfbStride <= 0 || L.prims <= 0 || L.prims * 3 > 64 || L.maxVerts <= 0) throw Fail{"bad xfb layout line"};
    xwords = (size_t)L.xfbStride / 4 * L.prims * L.maxVerts;
    xb = makeBuf(xwords * 4, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT);
    { Buf h = xb; sc.add([h]() mutable { freeBuf(h); }); }
    for (size_t i = 0; i < xwords; i++) ((uint32_t*)xb.p)[i] = SENTINEL;
    VkDeviceSize off = 0, sz = xwords * 4;
    vkCmdBindTransformFeedbackBuffersEXT(g_cb, 0, 1, &xb.b, &off, &sz);
    vkCmdBeginTransformFeedbackEXT(g_cb, 0, 0, nullptr, nullptr);
  }
  vkCmdDraw(g_cb, fsMode ? 3 : gsMode ? 3 * L.prims : 64, 1, 0, 0);
  if (gsMode) vkCmdEndTransformFeedbackEXT(g_cb, 0, 0, nullptr, nullptr);
  vkCmdEndRendering(g_cb);
  if (gsMode) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
  }
  for (int i = 0; i < nAtt; i++) {
    bars[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bars[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bars[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bars[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  }
  vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, nAtt, bars.data());
  for (int i = 0; i < nAtt; i++) {
    VkBufferImageCopy rg{};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {(uint32_t)fbw, (uint32_t)fbh, 1};
    vkCmdCopyImageToBuffer(g_cb, T.img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, T.rb[i].b, 1, &rg);
  }
  std::vector<VkBufferMemoryBarrier> bb(nAtt);
  for (int i = 0; i < nAtt; i++) {
    bb[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb[i].srcQueueFamilyIndex = bb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[i].buffer = T.rb[i].b; bb[i].offset = 0; bb[i].size = VK_WHOLE_SIZE;
  }
  vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                       nAtt, bb.data(), 0, nullptr);
  VKC(vkEndCommandBuffer(g_cb));
  submitWait();
  if (gsMode) {   // the captured vertex records are the result; the dummy attachment is never written
    std::vector<std::vector<uint32_t>> out(1);
    out[0].resize(xwords); memcpy(out[0].data(), xb.p, xwords * 4);
    return out;
  }
  std::vector<std::vector<uint32_t>> out(nAtt);
  size_t n = (size_t)fbw * fbh * 4;
  for (int i = 0; i < nAtt; i++) {
    out[i].resize(n); memcpy(out[i].data(), T.rb[i].p, n * 4);
  }
  return out;
}

// ---------------------------------------------------------------- compare
struct Cmp { uint64_t values = 0, exact = 0, close = 0, bad = 0; double maxd = 0; std::string first = "-"; };

static void compare(const std::vector<std::vector<uint32_t>>& A, const std::vector<std::vector<uint32_t>>& B,
                    int w, Cmp& c) {
  for (size_t rt = 0; rt < A.size(); rt++)
    for (size_t i = 0; i < A[rt].size(); i++) {
      uint32_t a = A[rt][i], b = B[rt][i];
      float fa = U2F(a), fb = U2F(b);
      c.values++;
      bool ex;
      if (a == SENTINEL || b == SENTINEL) ex = a == b;
      else ex = a == b || (std::isnan(fa) && std::isnan(fb));
      if (ex) { c.exact++; continue; }
      bool bad;
      if (std::isnan(fa) || std::isnan(fb) || std::isinf(fa) || std::isinf(fb)) bad = true;
      else {
        double d = std::fabs((double)fa - (double)fb);
        double tol = 1e-5 * std::max({1.0, (double)std::fabs(fa), (double)std::fabs(fb)});
        c.maxd = std::max(c.maxd, d);
        bad = d > tol;
      }
      if (bad) {
        if (c.bad == 0) {
          size_t px = i / 4, comp = i % 4;
          char buf[160];
          snprintf(buf, sizeof buf, "rt%zu:%zu,%zu,%zu:a=%.9g:b=%.9g", rt, px % (size_t)w, px / (size_t)w, comp, (double)fa, (double)fb);
          c.first = buf;
        }
        c.bad++;
      } else c.close++;
    }
}

static void dumpRaw(const Job& j, const char* side, const std::vector<std::vector<uint32_t>>& d) {
  if (g_dumpDir.empty()) return;
  for (size_t i = 0; i < d.size(); i++) {
    std::string p = g_dumpDir + "/" + j.id + "_" + side + "_rt" + std::to_string(i) + ".bin";
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) continue;
    fwrite(d[i].data(), 4, d[i].size(), f); fclose(f);
  }
}

static void runJob(const Job& j) {
  if (j.mode != "fs" && j.mode != "vs" && j.mode != "gs") throw Fail{"bad mode " + j.mode};
  Layout LA, LB;
  try { LA = parseLayout(j.alay); } catch (Fail& f) { throw Fail{"A: " + f.msg}; }
  try { LB = parseLayout(j.blay); } catch (Fail& f) { throw Fail{"B: " + f.msg}; }
  int w = 64, h = 64;
  if (j.mode == "vs") { w = LA.fbw > 0 ? LA.fbw : 64; h = 64; }
  std::vector<std::vector<uint32_t>> RA, RB;
  try { RA = renderSide(j, LA, j.aspv, w, h); } catch (Fail& f) { throw Fail{"A: " + f.msg}; }
  dumpRaw(j, "A", RA);
  try { RB = renderSide(j, LB, j.bspv, w, h); } catch (Fail& f) { throw Fail{"B: " + f.msg}; }
  dumpRaw(j, "B", RB);
  if (RA.size() != RB.size()) throw Fail{"attachment count differs between A and B"};
  Cmp c; compare(RA, RB, w, c);
  writeResult(j.id, c.bad ? "differ" : "equal", c.values, c.exact, c.close, c.bad, c.maxd, c.first, "");
}

// ---------------------------------------------------------------- init
static void initVulkan() {
  HMODULE dll = LoadLibraryA("vulkan-1.dll");
  if (!dll) throw Fail{"cannot load vulkan-1.dll"};
  gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vkGetInstanceProcAddr");
  if (!gipa) throw Fail{"no vkGetInstanceProcAddr"};
#define LG(n) vk##n = (PFN_vk##n)gipa(nullptr, "vk" #n);
  GLOBAL_FNS(LG)
#undef LG
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "vkdiff"; app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
  const char* iext[] = {"VK_EXT_debug_utils"};
  if (g_validate) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers;
                    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = iext; }
  VKC(vkCreateInstance(&ici, nullptr, &g_inst));
#define LI(n) vk##n = (PFN_vk##n)gipa(g_inst, "vk" #n);
  INST_FNS(LI)
#undef LI
  if (g_validate) {
    pfnCreateMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)gipa(g_inst, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = dbgCb;
    VkDebugUtilsMessengerEXT m; VKC(pfnCreateMessenger(g_inst, &mi, nullptr, &m));
  }
  uint32_t n = 0; vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(g_inst, &n, pds.data());
  g_pd = VK_NULL_HANDLE;
  for (auto pd : pds) {
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
    fprintf(stderr, "device: %s (type %d)\n", p.deviceName, (int)p.deviceType);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_pd = pd; g_maxUbo = p.limits.maxUniformBufferRange; break; }
  }
  if (!g_pd) throw Fail{"no CPU (lavapipe) Vulkan device found; refusing to use a real GPU"};
  if (g_maxUbo < 65536) fprintf(stderr, "warning: maxUniformBufferRange %u < 65536, clamping\n", g_maxUbo);
  vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);

  VkPhysicalDeviceVulkan13Features a13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features a12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan11Features a11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  a12.pNext = &a13; a11.pNext = &a12;
  VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; have.pNext = &a11;
  vkGetPhysicalDeviceFeatures2(g_pd, &have);
  if (!a13.dynamicRendering) throw Fail{"dynamicRendering not supported"};
  VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  e13.dynamicRendering = VK_TRUE;
  e13.shaderDemoteToHelperInvocation = a13.shaderDemoteToHelperInvocation;
  e13.subgroupSizeControl = a13.subgroupSizeControl;
  VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  e12.shaderFloat16 = a12.shaderFloat16; e12.shaderInt8 = a12.shaderInt8;
  e12.pNext = &e13;
  VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  e11.shaderDrawParameters = a11.shaderDrawParameters;
  e11.pNext = &e12;
  VkPhysicalDeviceFeatures2 en{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; en.pNext = &e11;
  VkPhysicalDeviceFeatures& hf = have.features; VkPhysicalDeviceFeatures& ef = en.features;
#define F(x) ef.x = hf.x;
  F(geometryShader) F(shaderClipDistance) F(shaderCullDistance) F(imageCubeArray) F(sampleRateShading)
  F(robustBufferAccess) F(fragmentStoresAndAtomics) F(vertexPipelineStoresAndAtomics) F(shaderTessellationAndGeometryPointSize)
  F(shaderInt64) F(shaderFloat64) F(shaderInt16) F(shaderStorageImageExtendedFormats)
  F(shaderStorageImageReadWithoutFormat) F(shaderStorageImageWriteWithoutFormat) F(shaderImageGatherExtended)
  F(multiViewport) F(tessellationShader) F(fillModeNonSolid) F(independentBlend) F(largePoints)
#undef F
  if (!hf.geometryShader) fprintf(stderr, "warning: geometryShader not supported\n");

  uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, qf.data());
  uint32_t qfi = UINT32_MAX;
  for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
  if (qfi == UINT32_MAX) throw Fail{"no graphics queue"};
  float prio = 1.f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &en; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  // VK_EXT_transform_feedback: geometry-shader jobs capture the emitted vertices
  uint32_t nx = 0; vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &nx, nullptr);
  std::vector<VkExtensionProperties> xp(nx); vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &nx, xp.data());
  for (auto& e : xp) if (!strcmp(e.extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) g_xfb = true;
  static const char* kExt[] = {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
  VkPhysicalDeviceTransformFeedbackFeaturesEXT xf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
  if (g_xfb) {
    xf.transformFeedback = VK_TRUE; xf.pNext = en.pNext; en.pNext = &xf;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = kExt;
  }
  VKC(vkCreateDevice(g_pd, &dci, nullptr, &g_dev));
  gdpa = vkGetDeviceProcAddr;
#define LD(n) vk##n = (PFN_vk##n)gdpa(g_dev, "vk" #n);
  DEV_FNS(LD)
  if (g_xfb) { LD(CmdBindTransformFeedbackBuffersEXT) LD(CmdBeginTransformFeedbackEXT) LD(CmdEndTransformFeedbackEXT) }
#undef LD
  vkGetDeviceQueue(g_dev, qfi, 0, &g_q);
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qfi;
  VKC(vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = g_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VKC(vkAllocateCommandBuffers(g_dev, &cai, &g_cb));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VKC(vkCreateFence(g_dev, &fci, nullptr, &g_fence));
}

int main(int argc, char** argv) {
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  std::vector<std::string> pos; bool append = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--validate") g_validate = true;
    else if (a == "--append") append = true;
    else if (a == "--time") g_time = true;
    else if (a == "--dump" && i + 1 < argc) g_dumpDir = argv[++i];
    else pos.push_back(a);
  }
  if (pos.size() != 2) {
    fprintf(stderr, "usage: vkdiff jobs.tsv results.tsv [--validate] [--dump DIR] [--append] [--time]\n");
    return 2;
  }
  if (!getenv("VK_ICD_FILENAMES") || !getenv("VK_DRIVER_FILES")) {
    fprintf(stderr, "refusing to run: set VK_ICD_FILENAMES and VK_DRIVER_FILES to the lavapipe ICD json\n");
    return 2;
  }
  g_results = fopen(pos[1].c_str(), append ? "ab" : "wb");
  if (!g_results) { fprintf(stderr, "cannot open %s\n", pos[1].c_str()); return 2; }
  std::vector<uint8_t> jd;
  try { jd = readFile(pos[0]); } catch (Fail& f) { fprintf(stderr, "%s\n", f.msg.c_str()); return 2; }
  try { initVulkan(); } catch (Fail& f) { fprintf(stderr, "init failed: %s\n", f.msg.c_str()); return 2; }

  std::string text(jd.begin(), jd.end());
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(p, e - p); p = e + 1;
    while (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f; size_t s = 0;
    while (true) {
      size_t t = line.find('\t', s);
      if (t == std::string::npos) { f.push_back(line.substr(s)); break; }
      f.push_back(line.substr(s, t - s)); s = t + 1;
    }
    if (f.size() < 10) {
      writeResult(f[0], "error", 0, 0, 0, 0, 0, "-", "malformed job line (need 10 tab separated fields)");
      continue;
    }
    Job j;
    j.id = f[0]; j.mode = f[1]; j.hvs = f[2]; j.hgs = f[3]; j.hfs = f[4]; j.aspv = f[5]; j.alay = f[6];
    j.bspv = f[7]; j.blay = f[8]; j.seed = (uint32_t)strtoull(f[9].c_str(), nullptr, 0);
    g_case = j.id;
    LARGE_INTEGER t0, t1, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    bool lost = false;
    try {
      runJob(j);
    } catch (TimeoutEx&) {
      writeResult(j.id, "error", 0, 0, 0, 0, 0, "-", "timeout");
      fprintf(stderr, "timeout in case %s, exiting with code 3\n", j.id.c_str());
      fflush(stderr);
      TerminateProcess(GetCurrentProcess(), 3);
    } catch (Fail& fl) {
      writeResult(j.id, "error", 0, 0, 0, 0, 0, "-", fl.msg);
      if (fl.msg.find("DEVICE_LOST") != std::string::npos) lost = true;
    } catch (std::exception& ex) {
      writeResult(j.id, "error", 0, 0, 0, 0, 0, "-", std::string("exception: ") + ex.what());
    }
    QueryPerformanceCounter(&t1);
    if (g_time) fprintf(stderr, "job %s: %.1f ms\n", j.id.c_str(), 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart);
    if (lost) { fprintf(stderr, "device lost, exiting with code 4\n"); fflush(stderr); TerminateProcess(GetCurrentProcess(), 4); }
  }
  fclose(g_results);
  if (g_validate) fprintf(stderr, "validation errors: %d\n", g_valErrors);
  fflush(stderr);
  // Vulkan objects are not torn down: process exit reclaims them.
  return 0;
}
