// Star Road Recomp: NVN on Vulkan, object lifetime helpers (pools, textures, samplers, programs) (task-12).
// Our own code (D-008).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <unordered_set>

#include "nvn_core.h"

namespace nvn {

// ---- logging ------------------------------------------------------------------------------------------------
int g_logLevel = 1;
static std::mutex g_logLock;
static std::unordered_map<std::string, bool> g_once;

void Log(const char* fmt, ...) {
  if (g_logLevel < 1) return;
  char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> l(g_logLock);
  fprintf(stderr, "[nvn] %s\n", b);
}
void Err(const char* fmt, ...) {
  char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> l(g_logLock);
  fprintf(stderr, "[nvn] ERROR: %s\n", b);
}
void LogOnce(const char* key, const char* fmt, ...) {
  {
    std::lock_guard<std::mutex> l(g_logLock);
    if (g_once.count(key)) return;
    g_once[key] = true;
  }
  char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  Log("%s", b);
}

// ---- virtual GPU address space --------------------------------------------------------------------------------
static std::mutex g_regLock;
static std::map<uint64_t, Pool*> g_pools;       // va base -> pool
static uint64_t g_vaNext = kVaBase;
static std::unordered_map<uint64_t, std::shared_ptr<CmdList>> g_lists;
static uint64_t g_listNext = 0x10000;

uint64_t NextVaBase(uint64_t size) {
  std::lock_guard<std::mutex> l(g_regLock);
  uint64_t b = g_vaNext;
  uint64_t step = (size + 0xFFFFFFFull) & ~0xFFFFFFFull;      // 256 MiB granularity keeps pools apart
  g_vaNext += std::max<uint64_t>(step, 0x10000000ull) + 0x10000000ull;
  return b;
}
void RegisterPool(Pool* p) { std::lock_guard<std::mutex> l(g_regLock); g_pools[p->va] = p; }
void UnregisterPool(Pool* p) { std::lock_guard<std::mutex> l(g_regLock); g_pools.erase(p->va); }

Pool* PoolFromVa(uint64_t va, uint64_t* off) {
  std::lock_guard<std::mutex> l(g_regLock);
  auto it = g_pools.upper_bound(va);
  if (it == g_pools.begin()) return nullptr;
  --it;
  Pool* p = it->second;
  if (va - p->va > p->size) return nullptr;
  if (off) *off = va - p->va;
  return p;
}
uint8_t* HostPtr(uint64_t va, uint64_t size) {
  uint64_t off; Pool* p = PoolFromVa(va, &off);
  if (!p || !p->memory || size > p->size - off) return nullptr;
  return p->memory + off;
}
bool ResolveBuffer(uint64_t va, uint64_t size, VkBuffer* buf, uint64_t* off) {
  uint64_t o; Pool* p = PoolFromVa(va, &o);
  if (!p || p->buffer == VK_NULL_HANDLE || size > p->size - o) return false;
  *buf = p->buffer; *off = o; return true;
}
std::shared_ptr<CmdList> FindList(uint64_t id) {
  std::lock_guard<std::mutex> l(g_regLock);
  auto it = g_lists.find(id);
  return it == g_lists.end() ? nullptr : it->second;
}
uint64_t RegisterList(std::shared_ptr<CmdList> l) {
  std::lock_guard<std::mutex> lk(g_regLock);
  uint64_t id = g_listNext; g_listNext += 8; l->id = id; g_lists[id] = std::move(l); return id;
}
void ReleaseList(uint64_t id) { std::lock_guard<std::mutex> l(g_regLock); g_lists.erase(id); }

// ---- memory pools ---------------------------------------------------------------------------------------------
bool PoolCreate(Pool* p) {
  if (!g_vk.inited) { Err("PoolCreate: Vulkan not initialized"); return false; }
  p->va = NextVaBase(p->size);
  if (p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) { RegisterPool(p); return true; }   // no storage until mapped (not used by the game)
  if (!p->memory) { Err("memory pool without storage"); return false; }
  if (!g_vk.hasHostImport) { Err("memory pool needs VK_EXT_external_memory_host (mirror mode not implemented)"); return false; }
  VkDeviceSize al = g_vk.hostAlign ? g_vk.hostAlign : 4096;
  if (((uintptr_t)p->memory & (al - 1)) || (p->size & (al - 1))) { Err("memory pool storage must be %llu aligned (ptr %p size %llx)", (unsigned long long)al, p->memory, (unsigned long long)p->size); return false; }
  auto& F = g_vk.f;
  VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
  if (F.vkGetMemoryHostPointerPropertiesEXT(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p->memory, &hp) != VK_SUCCESS) {
    Err("vkGetMemoryHostPointerPropertiesEXT failed for %p", p->memory); return false;
  }
  VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
  eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.pNext = &eb; bi.size = p->size;
  bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
             VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (F.vkCreateBuffer(g_vk.dev, &bi, nullptr, &p->buffer) != VK_SUCCESS) { Err("pool buffer create failed (size %llx)", (unsigned long long)p->size); return false; }
  VkMemoryRequirements req; F.vkGetBufferMemoryRequirements(g_vk.dev, p->buffer, &req);
  uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits;
  uint32_t type = FindMemoryType(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type == UINT32_MAX) type = FindMemoryType(bits, 0);
  if (type == UINT32_MAX) { Err("no memory type for host import"); F.vkDestroyBuffer(g_vk.dev, p->buffer, nullptr); p->buffer = VK_NULL_HANDLE; return false; }
  VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
  imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT; imp.pHostPointer = p->memory;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.pNext = &imp; ai.allocationSize = std::max<VkDeviceSize>(req.size, p->size); ai.memoryTypeIndex = type;
  if (F.vkAllocateMemory(g_vk.dev, &ai, nullptr, &p->mem.mem) != VK_SUCCESS) { Err("host memory import failed"); F.vkDestroyBuffer(g_vk.dev, p->buffer, nullptr); p->buffer = VK_NULL_HANDLE; return false; }
  F.vkBindBufferMemory(g_vk.dev, p->buffer, p->mem.mem, 0);
  p->imported = true;
  RegisterPool(p);
  return true;
}

static std::unordered_set<const void*> g_live;          // pools and programs the guest has initialized and not finalized (guarded by the API lock)
void LiveAdd(const void* o) { g_live.insert(o); }
void LiveDel(const void* o) { g_live.erase(o); }
bool IsLive(const void* o) { return g_live.count(o) != 0; }

void PoolDestroy(Pool* p) {
  UnregisterPool(p);
  // Textures and buffers may outlive the pool they were made from (the game finalizes a pool first, then its objects). They keep a
  // pointer to this struct, which is never freed: it stays behind as an empty `dead` shell.
  for (TextureImpl* t : p->textures) if (t) t->pool = nullptr;
  p->textures.clear(); p->textures.shrink_to_fit(); p->memory = nullptr; p->dead = true;
  if (p->buffer) { VkBuffer b = p->buffer; VkDeviceMemory m = p->mem.mem; bool imp = p->imported;
    DeferUntil(TimelineLast(), [b, m, imp]() { (void)imp; g_vk.f.vkDestroyBuffer(g_vk.dev, b, nullptr); if (m) g_vk.f.vkFreeMemory(g_vk.dev, m, nullptr); });
  }
  p->buffer = VK_NULL_HANDLE; p->mem = MemAlloc{};
}

// ---- textures -------------------------------------------------------------------------------------------------
static VkSampleCountFlagBits Samples(uint32_t n) {
  switch (n) { case 2: return VK_SAMPLE_COUNT_2_BIT; case 4: return VK_SAMPLE_COUNT_4_BIT; case 8: return VK_SAMPLE_COUNT_8_BIT; case 16: return VK_SAMPLE_COUNT_16_BIT; default: return VK_SAMPLE_COUNT_1_BIT; }
}

bool TextureCreate(TextureG* g, TextureImpl* t) {
  auto& F = g_vk.f;
  int srFmt = 0, srgb = 0;
  t->vkFormat = VkFormatFromNvn(g->format, &srFmt, &srgb, &t->aspect);
  t->nvnFormat = g->format; t->sr_fmt = srFmt; t->sr_srgb = srgb;
  if (t->vkFormat == VK_FORMAT_UNDEFINED) { Err("texture: unsupported NVN format 0x%x", g->format); return false; }
  t->width = std::max(1u, g->width); t->height = std::max(1u, g->height);
  VkImageType type = VK_IMAGE_TYPE_2D; uint32_t layers = 1, depth = 1; VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  switch (g->target) {
    case NVN_TEXTURE_TARGET_1D: type = VK_IMAGE_TYPE_1D; t->height = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: type = VK_IMAGE_TYPE_1D; t->height = 1; layers = std::max(1u, g->depth); break;
    case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_TYPE_3D; depth = std::max(1u, g->depth); t->is3D = true; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: layers = std::max(1u, g->depth); break;
    case NVN_TEXTURE_TARGET_CUBEMAP: layers = 6; t->isCube = true; flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: layers = std::max(1u, g->depth); if (layers % 6) layers *= 6; t->isCube = true; flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case NVN_TEXTURE_TARGET_BUFFER: Err("texture target BUFFER is not an image (texel buffers are not supported yet)"); return false;
    default: break;   // 2D, rectangle, multisample
  }
  t->layers = layers; t->depth = depth; t->levels = std::max(1u, g->levels); t->samples = std::max(1u, g->samples);
  if (t->samples > 1) t->levels = 1;                 // a multisample image has one mip level
  VkFormatProperties fp; F.vkGetPhysicalDeviceFormatProperties(g_vk.phys, t->vkFormat, &fp);
  VkFormatFeatureFlags feat = fp.optimalTilingFeatures;
  t->optimalFeatures = feat;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if ((feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (t->samples <= 1 || g_vk.feat.features.shaderStorageImageMultisample)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  t->renderTarget = (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.flags = flags; ci.imageType = type; ci.format = t->vkFormat;
  ci.extent = {t->width, t->height, depth}; ci.mipLevels = t->levels; ci.arrayLayers = layers;
  ci.samples = Samples(g->samples); ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult r = F.vkCreateImage(g_vk.dev, &ci, nullptr, &t->image);
  if (r != VK_SUCCESS) { Err("vkCreateImage failed (%d) fmt 0x%x %ux%u", r, g->format, t->width, t->height); return false; }
  VkMemoryRequirements req; F.vkGetImageMemoryRequirements(g_vk.dev, t->image, &req);
  if (!AllocMemory(MemKind::Device, req, &t->mem)) { Err("texture memory allocation failed"); F.vkDestroyImage(g_vk.dev, t->image, nullptr); t->image = VK_NULL_HANDLE; return false; }
  F.vkBindImageMemory(g_vk.dev, t->image, t->mem.mem, t->mem.offset);
  return true;
}

void TextureDestroy(TextureImpl* t) {
  auto views = std::move(t->views); VkImage im = t->image; MemAlloc mem = t->mem; bool owned = t->owned; uint64_t last = t->lastUse;
  t->image = VK_NULL_HANDLE; t->mem = MemAlloc{};
  DeferUntil(last, [views, im, mem, owned]() mutable {
    for (auto& kv : views) g_vk.f.vkDestroyImageView(g_vk.dev, kv.second, nullptr);
    if (owned && im) g_vk.f.vkDestroyImage(g_vk.dev, im, nullptr);
    FreeMemory(mem);
  });
}

VkImageView GetView(TextureImpl* t, const TextureG* g, const TextureView* v, bool att) {
  uint32_t flags = v ? v->flags : 0;
  uint32_t target = (flags & 1) ? v->target : g->target;
  uint32_t fmt = (flags & 2) ? v->format : g->format;
  uint8_t swz[4]; for (int i = 0; i < 4; i++) swz[i] = (flags & 4) ? v->swz[i] : (uint8_t)g->swz[i];
  uint32_t minL = (flags & 8) ? v->minLevel : 0, nL = (flags & 8) ? v->numLevels : t->levels;
  uint32_t minY = (flags & 16) ? v->minLayer : 0, nY = (flags & 16) ? v->numLayers : t->layers;
  if (att) nL = 1;
  if (minL >= t->levels) minL = t->levels - 1;
  nL = std::min(nL, t->levels - minL); nY = std::max(1u, std::min(nY, t->layers - std::min(minY, t->layers - 1)));
  uint64_t key = (uint64_t)target | (uint64_t)fmt << 8 | (uint64_t)minL << 16 | (uint64_t)nL << 22 | (uint64_t)minY << 28 | (uint64_t)nY << 40 |
                 (uint64_t)(swz[0] * 1 + swz[1] * 6 + swz[2] * 36 + swz[3] * 216) << 50 | (uint64_t)att << 63;
  auto it = t->views.find(key);
  if (it != t->views.end()) return it->second;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = t->image;
  VkImageViewType vt = VK_IMAGE_VIEW_TYPE_2D;
  switch (target) {
    case NVN_TEXTURE_TARGET_1D: vt = VK_IMAGE_VIEW_TYPE_1D; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case NVN_TEXTURE_TARGET_3D: vt = VK_IMAGE_VIEW_TYPE_3D; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case NVN_TEXTURE_TARGET_CUBEMAP: vt = VK_IMAGE_VIEW_TYPE_CUBE; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
    default: break;
  }
  if (att && (vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  if (att && vt == VK_IMAGE_VIEW_TYPE_3D) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.viewType = vt;
  int sf = 0, sg = 0; VkImageAspectFlags asp = t->aspect;
  vi.format = (flags & 2) ? VkFormatFromNvn(fmt, &sf, &sg, &asp) : t->vkFormat;
  if (vi.format == VK_FORMAT_UNDEFINED) vi.format = t->vkFormat;
  if (!att && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) asp = VK_IMAGE_ASPECT_DEPTH_BIT;       // sample depth, not stencil
  else if (!att && asp == VK_IMAGE_ASPECT_STENCIL_BIT) asp = VK_IMAGE_ASPECT_STENCIL_BIT;
  vi.subresourceRange = {asp, minL, nL, minY, nY};
  if (vt == VK_IMAGE_VIEW_TYPE_3D) { vi.subresourceRange.baseArrayLayer = 0; vi.subresourceRange.layerCount = 1; }
  vi.components = {att ? VK_COMPONENT_SWIZZLE_IDENTITY : SwizzleFromNvn(swz[0], VK_COMPONENT_SWIZZLE_R),
                   att ? VK_COMPONENT_SWIZZLE_IDENTITY : SwizzleFromNvn(swz[1], VK_COMPONENT_SWIZZLE_G),
                   att ? VK_COMPONENT_SWIZZLE_IDENTITY : SwizzleFromNvn(swz[2], VK_COMPONENT_SWIZZLE_B),
                   att ? VK_COMPONENT_SWIZZLE_IDENTITY : SwizzleFromNvn(swz[3], VK_COMPONENT_SWIZZLE_A)};
  // NVN default swizzle is R,G,B,A (values 2,3,4,5): identity
  if (!att) {
    auto id = [](VkComponentSwizzle s, VkComponentSwizzle i) { return s == i ? VK_COMPONENT_SWIZZLE_IDENTITY : s; };
    vi.components.r = id(vi.components.r, VK_COMPONENT_SWIZZLE_R); vi.components.g = id(vi.components.g, VK_COMPONENT_SWIZZLE_G);
    vi.components.b = id(vi.components.b, VK_COMPONENT_SWIZZLE_B); vi.components.a = id(vi.components.a, VK_COMPONENT_SWIZZLE_A);
  }
  VkImageView out = VK_NULL_HANDLE;
  VkResult r = g_vk.f.vkCreateImageView(g_vk.dev, &vi, nullptr, &out);
  if (r != VK_SUCCESS) { Err("vkCreateImageView failed (%d)", r); return VK_NULL_HANDLE; }
  t->views[key] = out;
  return out;
}

// ---- samplers -------------------------------------------------------------------------------------------------
static VkSamplerAddressMode Wrap(uint32_t w) {
  switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // CLAMP, CLAMP_TO_EDGE
  }
}
static VkCompareOp CmpOp(uint32_t f) {
  switch (f) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER; case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL; case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER; case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL; default: return VK_COMPARE_OP_ALWAYS;
  }
}

bool SamplerCreate(SamplerImpl* s) {
  const SamplerBuilderG& d = s->desc;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  bool minLin = d.minFilter == NVN_MIN_FILTER_LINEAR || d.minFilter == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || d.minFilter == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR;
  bool mipLin = d.minFilter == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || d.minFilter == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR;
  bool mips = d.minFilter >= NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST;
  si.magFilter = d.magFilter == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.minFilter = minLin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.mipmapMode = mipLin ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = Wrap(d.wrapS); si.addressModeV = Wrap(d.wrapT); si.addressModeW = Wrap(d.wrapR);
  si.mipLodBias = d.lodBias; si.minLod = d.minLod; si.maxLod = mips ? d.maxLod : 0.25f;
  if (d.maxAniso > 1.0f && g_vk.feat.features.samplerAnisotropy) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = std::min(d.maxAniso, g_vk.props.limits.maxSamplerAnisotropy); }
  if (d.compareMode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) { si.compareEnable = VK_TRUE; si.compareOp = CmpOp(d.compareFunc); }
  si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  float mx = d.border[0] + d.border[1] + d.border[2] + d.border[3];
  if (mx > 3.99f) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  else if (d.border[3] > 0.99f && mx < 1.01f) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  VkSamplerReductionModeCreateInfo rm{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
  if (d.reduction != NVN_SAMPLER_REDUCTION_AVERAGE && g_vk.hasMinMax) {
    rm.reductionMode = d.reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
    si.pNext = &rm;
  }
  return g_vk.f.vkCreateSampler(g_vk.dev, &si, nullptr, &s->sampler) == VK_SUCCESS;
}
void SamplerDestroy(SamplerImpl* s) {
  VkSampler h = s->sampler; s->sampler = VK_NULL_HANDLE;
  DeferUntil(TimelineLast(), [h]() { g_vk.f.vkDestroySampler(g_vk.dev, h, nullptr); });
}

// ---- programs -------------------------------------------------------------------------------------------------
bool ProgramSetShadersImpl(ProgramImpl* p, uint32_t n, const uint64_t* data, const void* const* control) {
  p->stages.clear(); for (auto& s : p->byStage) s = nullptr;
  bool ok = true;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t* ctl = (const uint8_t*)control[i];
    if (!ctl || *(const uint32_t*)ctl != 0x98761234u) { LogOnce("badctl", "ProgramSetShaders: control section %u has no 0x98761234 magic", i); ok = false; continue; }
    uint32_t dsz = *(const uint32_t*)(ctl + 0x704);
    const uint8_t* bytes = HostPtr(data[i], dsz);
    if (!bytes) { LogOnce("baddata", "ProgramSetShaders: shader data address 0x%llx not inside a pool", (unsigned long long)data[i]); ok = false; continue; }
    std::string key = Sha256Hex(bytes, dsz);
    StageProgram* sp = ShaderCacheGet(key);
    p->stages.push_back(sp);
    if (!sp->ok) {
      ok = false;
      // NVN_DUMP_MISSING=<dir>: write every stage that is not in the cache as <key>.data and <key>.ctrl, the corpus layout tools/shaderxlate reads.
      static const char* dumpDir = getenv("NVN_DUMP_MISSING");
      if (dumpDir && *dumpDir && sp->reason == "not in shader cache") {
        static std::unordered_set<std::string> dumped;
        if (dumped.insert(key).second) {
          for (int k = 0; k < 2; k++) {
            std::string path = std::string(dumpDir) + "/" + key + (k ? ".ctrl" : ".data");
            if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(k ? (const void*)ctl : (const void*)bytes, 1, k ? 0x880 : dsz, f); fclose(f); }
          }
          LogOnce(("dump" + key).c_str(), "ProgramSetShaders: stage %s not in the cache, dumped to %s", key.c_str(), dumpDir);
        }
      }
      continue;
    }
    if (sp->man.stage >= 0 && sp->man.stage < 6) p->byStage[sp->man.stage] = sp;
  }
  p->isCompute = p->byStage[NVN_SHADER_STAGE_COMPUTE] != nullptr;
  p->complete = ok && (p->byStage[NVN_SHADER_STAGE_VERTEX] || p->isCompute);
  return p->complete;
}

}  // namespace nvn
