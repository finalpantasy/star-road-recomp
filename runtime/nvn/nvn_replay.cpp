// Star Road Recomp: NVN on Vulkan, command replay, pipelines, queue, window (task-12). Our own code (D-008).
//
// Command buffers record NVN calls into a CmdList. Queue submission replays the lists into one Vulkan command buffer with a
// state machine that mirrors NVN's bound state; draws build (and cache) a pipeline from the current state and the shader cache
// manifest (docs/SHADER_CACHE.md). Every image lives in GENERAL layout; hazards use global barriers at render pass boundaries
// and at NVN Barrier calls.
#include <algorithm>
#include <cstdio>
#include <mutex>

#include "../gfx/texture/texture.h"
#include "nvn_core.h"

namespace nvn {

static Stats g_stats;
Stats& GetStats() { return g_stats; }
static bool g_windowHidden = true;
void SetWindowHidden(bool h) { g_windowHidden = h; }

// ---- scratch memory (host visible, per submission) --------------------------------------------------------------
static std::vector<Buf> g_freeChunks;
struct Scratch {
  std::vector<Buf> used; size_t cur = SIZE_MAX; VkDeviceSize off = 0;
  bool Alloc(VkDeviceSize size, VkDeviceSize align, VkBuffer* b, VkDeviceSize* o, void** ptr) {
    align = std::max<VkDeviceSize>(align, 16);
    if (cur != SIZE_MAX) {
      VkDeviceSize a = (off + align - 1) / align * align;
      if (a + size <= used[cur].size) { *b = used[cur].buf; *o = a; *ptr = (uint8_t*)used[cur].map + a; off = a + size; return true; }
    }
    Buf nb; bool got = false;
    {
      std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
      for (size_t i = 0; i < g_freeChunks.size(); i++)
        if (g_freeChunks[i].size >= size) { nb = g_freeChunks[i]; g_freeChunks.erase(g_freeChunks.begin() + (ptrdiff_t)i); got = true; break; }
    }
    if (!got) {
      VkDeviceSize sz = std::max<VkDeviceSize>(size, 4u << 20);
      if (!CreateBuf(&nb, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemKind::HostVisible)) return false;
    }
    used.push_back(nb); cur = used.size() - 1; off = size;
    *b = nb.buf; *o = 0; *ptr = nb.map; return true;
  }
};

// ---- descriptor pools (per submission) ---------------------------------------------------------------------------
struct DescPools {
  std::vector<VkDescriptorPool> pools;
  VkDescriptorSet Alloc(VkDescriptorSetLayout l) {
    for (int attempt = 0; attempt < 2; attempt++) {
      if (!pools.empty()) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pools.back(); ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
        VkDescriptorSet s;
        if (g_vk.f.vkAllocateDescriptorSets(g_vk.dev, &ai, &s) == VK_SUCCESS) return s;
      }
      VkDescriptorPoolSize sz[6] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
      VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pi.maxSets = 1024; pi.poolSizeCount = 6; pi.pPoolSizes = sz;
      VkDescriptorPool dp;
      if (g_vk.f.vkCreateDescriptorPool(g_vk.dev, &pi, nullptr, &dp) != VK_SUCCESS) return VK_NULL_HANDLE;
      pools.push_back(dp);
    }
    return VK_NULL_HANDLE;
  }
};

// ---- caches -----------------------------------------------------------------------------------------------------
static VkPipelineCache g_pcache = VK_NULL_HANDLE;
static std::unordered_map<std::string, VkPipeline> g_pipes;
static std::unordered_map<std::string, VkPipelineLayout> g_layouts;
static VkDescriptorSetLayout g_emptySet = VK_NULL_HANDLE;
static VkBuffer g_nullVb = VK_NULL_HANDLE; static Buf g_nullVbBuf;
struct Dummy { VkImage img = VK_NULL_HANDLE; MemAlloc mem; VkImageView view = VK_NULL_HANDLE; };
static Dummy g_dummy[7];                 // by manifest dim: 1d 2d 3d cube 2d_array cube_array buffer
static VkSampler g_dummySampler = VK_NULL_HANDLE;
static bool g_dummyInit = false;

void ReplayShutdown() {
  if (!g_vk.inited) return;
  auto& F = g_vk.f;
  F.vkDeviceWaitIdle(g_vk.dev);
  for (auto& kv : g_pipes) if (kv.second) F.vkDestroyPipeline(g_vk.dev, kv.second, nullptr);
  g_pipes.clear();
  for (auto& kv : g_layouts) if (kv.second) F.vkDestroyPipelineLayout(g_vk.dev, kv.second, nullptr);
  g_layouts.clear();
  if (g_emptySet) F.vkDestroyDescriptorSetLayout(g_vk.dev, g_emptySet, nullptr);
  g_emptySet = VK_NULL_HANDLE;
  if (g_pcache) F.vkDestroyPipelineCache(g_vk.dev, g_pcache, nullptr);
  g_pcache = VK_NULL_HANDLE;
  if (g_nullVbBuf.buf) DestroyBuf(&g_nullVbBuf);
  g_nullVbBuf = Buf{}; g_nullVb = VK_NULL_HANDLE;
  for (auto& d : g_dummy) {
    if (d.view) F.vkDestroyImageView(g_vk.dev, d.view, nullptr);
    if (d.img) F.vkDestroyImage(g_vk.dev, d.img, nullptr);
    if (d.mem.mem) FreeMemory(d.mem);
    d = Dummy{};
  }
  if (g_dummySampler) F.vkDestroySampler(g_vk.dev, g_dummySampler, nullptr);
  g_dummySampler = VK_NULL_HANDLE; g_dummyInit = false;
  for (auto& b : g_freeChunks) DestroyBuf(&b);
  g_freeChunks.clear();
  ShaderCacheReleaseVk();
}

static VkShaderStageFlagBits StageBit(int s) {
  switch (s) {
    case NVN_SHADER_STAGE_VERTEX: return VK_SHADER_STAGE_VERTEX_BIT; case NVN_SHADER_STAGE_FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case NVN_SHADER_STAGE_GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT; case NVN_SHADER_STAGE_TESS_CONTROL: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case NVN_SHADER_STAGE_TESS_EVALUATION: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; default: return VK_SHADER_STAGE_COMPUTE_BIT;
  }
}
static uint32_t SetOfStage(int s) { return s == NVN_SHADER_STAGE_COMPUTE ? 0 : (uint32_t)s; }

static void EnsureGlobals() {
  if (g_pcache) return;
  VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  g_vk.f.vkCreatePipelineCache(g_vk.dev, &pc, nullptr, &g_pcache);
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  g_vk.f.vkCreateDescriptorSetLayout(g_vk.dev, &li, nullptr, &g_emptySet);
  // stands in for every unbound vertex, uniform and storage buffer, so it carries all three usages
  CreateBuf(&g_nullVbBuf, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemKind::HostVisible);
  float* f = (float*)g_nullVbBuf.map; for (int i = 0; i < 64; i++) f[i] = (i & 3) == 3 ? 1.0f : 0.0f;
  g_nullVb = g_nullVbBuf.buf;
}

static VkDescriptorType TexDescType(const ManifestTex& t) {
  switch (t.res) {
    case RES_SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case RES_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     // texel buffers were refused by ShaderCacheCheckDevice
  }
}

// Phase A modules ask for subgroup size 32 (Maxwell warps). Devices that run another size need VK_EXT_subgroup_size_control.
static bool WantSubgroup32(const StageProgram* sp, VkShaderStageFlagBits bit) {
  return sp->man.subgroup32 && g_vk.p11.subgroupSize != 32 && g_vk.f13.subgroupSizeControl && g_vk.p13.minSubgroupSize <= 32 &&
         g_vk.p13.maxSubgroupSize >= 32 && (g_vk.p13.requiredSubgroupSizeStages & bit) != 0;
}

static bool EnsureStage(StageProgram* sp) {
  if (!sp->ok) return false;
  if (!ShaderCacheCheckDevice(sp)) return false;
  if (sp->module && sp->setLayout) return true;
  auto& F = g_vk.f;
  if (!sp->module) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sp->spv.size() * 4; mi.pCode = sp->spv.data();
    if (F.vkCreateShaderModule(g_vk.dev, &mi, nullptr, &sp->module) != VK_SUCCESS) { sp->ok = false; sp->reason = "vkCreateShaderModule failed"; LogOnce(sp->key.c_str(), "shader %s: module creation failed", sp->key.c_str()); return false; }
  }
  std::vector<VkDescriptorSetLayoutBinding> b;
  VkShaderStageFlags st = StageBit(sp->man.stage);
  if (sp->man.supportBinding >= 0) b.push_back({(uint32_t)sp->man.supportBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st, nullptr});
  for (auto& u : sp->man.ubos) b.push_back({u.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st, nullptr});
  for (auto& t : sp->man.texs) b.push_back({t.binding, TexDescType(t), std::max(1u, t.arrayLen), st, nullptr});
  for (auto& i : sp->man.imgs) b.push_back({i.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, st, nullptr});
  for (auto& s : sp->man.ssbos) b.push_back({s.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr});
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = (uint32_t)b.size(); li.pBindings = b.data();
  if (F.vkCreateDescriptorSetLayout(g_vk.dev, &li, nullptr, &sp->setLayout) != VK_SUCCESS) { sp->ok = false; return false; }
  return true;
}

bool ShaderStageReady(StageProgram* sp) { return EnsureStage(sp); }

static VkPipelineLayout GetLayout(ProgramImpl* p) {
  uint32_t nsets = 0;
  VkDescriptorSetLayout sets[6];
  for (auto& s : sets) s = g_emptySet;
  std::string key;
  for (int s = 0; s < 6; s++) {
    StageProgram* sp = p->byStage[s];
    if (!sp) continue;
    uint32_t idx = SetOfStage(s);
    sets[idx] = sp->setLayout; nsets = std::max(nsets, idx + 1);
  }
  key.assign((const char*)sets, nsets * sizeof(VkDescriptorSetLayout));
  auto it = g_layouts.find(key);
  if (it != g_layouts.end()) return it->second;
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = nsets; pl.pSetLayouts = sets;
  VkPipelineLayout out = VK_NULL_HANDLE;
  g_vk.f.vkCreatePipelineLayout(g_vk.dev, &pl, nullptr, &out);
  g_layouts[key] = out;
  return out;
}

// ---- replay state -----------------------------------------------------------------------------------------------
struct RtBind { TextureG* tex = nullptr; TextureView view{}; };

struct RS {
  VkCommandBuffer cb = VK_NULL_HANDLE; uint64_t value = 0;
  Scratch* scratch = nullptr; DescPools* dpools = nullptr; std::vector<std::shared_ptr<CmdList>>* keep = nullptr;
  // bound NVN state
  BlendStateG blend[8]; ColorStateG colorState{0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS, 0}; ChannelMaskG chan{0xFFFFFFFFu};
  PolygonStateG poly{NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
  DepthStencilG ds{}; MultisampleG ms{};
  VertexAttribG attr[16]; bool attrSet[16] = {}; VertexStreamG stream[16];
  ProgramImpl* prog = nullptr; uint32_t progStages = 0;
  struct Range { uint64_t addr = 0, size = 0; };
  Range vb[16], ub[6][16], sb[6][16];
  uint32_t c2[6][0x400] = {};                    // c[2] handle words per stage
  uint64_t img[6][32] = {};
  TexPool* texPool = nullptr; SampPool* sampPool = nullptr;
  // render targets
  uint32_t nColor = 0; RtBind color[8]; RtBind depth; bool rtDirty = true, rendering = false; VkExtent2D area{};
  uint32_t renderLayers = 1;
  // dynamic and misc
  int32_t viewport[4] = {0, 0, 0, 0}; int32_t scissor[4] = {0, 0, 0, 0}; bool vpSet = false, scSet = false;
  float depthN = 0.0f, depthF = 1.0f; bool depthBoundsOn = false; float depthBounds[2] = {0, 1};
  bool depthClamp = false, discard = false, primRestart = false; uint32_t sampleMask = 0xFFFFFFFFu, patchSize = 3;
  float lineWidth = 1.0f, polyFactor = 0, polyUnits = 0, polyClamp = 0;
  uint32_t stRef[2] = {0, 0}, stMask[2] = {0xFF, 0xFF}, stWrite[2] = {0xFF, 0xFF};
  uint32_t depthMode = NVN_DEPTH_MODE_NEAR_IS_ZERO;
  uint32_t originMode = NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
  bool pipeDirty = true; VkPipeline curPipe = VK_NULL_HANDLE;
  Device* dev = nullptr; QueueImpl* q = nullptr;
  RS() { for (auto& b : blend) { b = BlendStateG{}; b.srcRGB = b.srcA = NVN_BLEND_FUNC_ONE; b.dstRGB = b.dstA = NVN_BLEND_FUNC_ZERO; b.eq = NVN_BLEND_EQUATION_ADD | (NVN_BLEND_EQUATION_ADD << 3); }
         ds.depthFunc = NVN_DEPTH_FUNC_LESS; ds.flags = 0; ds.func = NVN_STENCIL_FUNC_ALWAYS | (NVN_STENCIL_FUNC_ALWAYS << 4);
         ds.opF = NVN_STENCIL_OP_KEEP | (NVN_STENCIL_OP_KEEP << 4); ds.opF2 = NVN_STENCIL_OP_KEEP; ds.opB = ds.opF; ds.opB2 = ds.opF2;
         ms.samples = 0; for (auto& a : attr) a = VertexAttribG{}; for (auto& s : stream) s = VertexStreamG{0, 0}; }
};

static void GlobalBarrier(VkCommandBuffer cb) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  g_vk.f.vkCmdPipelineBarrier2(cb, &di);
}

static void EndRendering(RS& s) {
  if (!s.rendering) return;
  g_vk.f.vkCmdEndRendering(s.cb); s.rendering = false;
  GlobalBarrier(s.cb);
}

static void ToGeneral(RS& s, TextureImpl* t) {
  if (t->layoutInit || !t->image) return;
  EndRendering(s);
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_NONE; b.srcAccessMask = 0;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = t->image;
  b.subresourceRange = {t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  g_vk.f.vkCmdPipelineBarrier2(s.cb, &di);
  t->layoutInit = true;
}

// Copy swizzled guest texels into the image (task-11 deswizzle, decode when the GPU cannot take the format).
static void UploadTexture(RS& s, TextureImpl* t, TextureG* g) {
  Pool* pool = t->pool;
  t->uploaded = true;                 // one attempt per guest write; FlushMappedRange clears it
  if (!pool || !pool->memory) return;
  if (IsDepthFormat(t->vkFormat)) return;
  sr_tex_format fmt;
  if (sr_tex_format_from_nvn(g->format, &fmt) != SR_TEX_OK) { LogOnce("upfmt", "texture upload: format 0x%x not handled by task-11", g->format); return; }
  sr_tex_surface_desc d{};
  d.format = fmt; d.width = t->width; d.height = t->height; d.depth = t->is3D ? t->depth : 1;
  d.layers = t->is3D ? 1 : t->layers; d.levels = t->levels; d.block_height_log2 = SR_TEX_BH_AUTO; d.block_depth_log2 = SR_TEX_BH_AUTO;
  d.dim = t->is3D ? SR_TEX_DIM_3D : t->isCube ? SR_TEX_DIM_CUBE : SR_TEX_DIM_2D;
  sr_tex_plan plan;
  if (sr_tex_plan_build(&d, &plan) != SR_TEX_OK) { LogOnce("upplan", "texture upload: plan failed for %ux%u fmt 0x%x", t->width, t->height, g->format); return; }
  uint64_t avail = pool->size > t->poolOffset ? pool->size - t->poolOffset : 0;
  bool isAstc = fmt.fmt >= SR_TEX_FMT_ASTC_4x4 && fmt.fmt <= SR_TEX_FMT_ASTC_12x12;
  bool astcNative = isAstc && t->vkFormat != VK_FORMAT_R8G8B8A8_UNORM && t->vkFormat != VK_FORMAT_R8G8B8A8_SRGB;
  sr_tex_gpu_caps caps{}; caps.astc = astcNative ? 1 : 0; caps.bc7 = 1;
  sr_tex_upload up{};
  int rc = sr_tex_prepare_upload(&plan, pool->memory + t->poolOffset, (size_t)std::min<uint64_t>(avail, plan.total_size),
                                 isAstc && !astcNative ? SR_TEX_TARGET_RGBA8 : SR_TEX_TARGET_NATIVE, caps, &up);
  if (rc != SR_TEX_OK && rc != SR_TEX_E_CORRUPT) { LogOnce("uprc", "texture upload failed (%d) fmt 0x%x %ux%u", rc, g->format, t->width, t->height); sr_tex_upload_free(&up); return; }
  VkBuffer sb; VkDeviceSize so; void* sp;
  if (!s.scratch->Alloc(up.data_size, 16, &sb, &so, &sp)) { sr_tex_upload_free(&up); return; }
  memcpy(sp, up.data, up.data_size);
  std::vector<VkBufferImageCopy> regs;
  for (uint32_t i = 0; i < up.region_count; i++) {
    const sr_tex_region& r = up.regions[i];
    VkBufferImageCopy c{};
    c.bufferOffset = so + r.buffer_offset; c.bufferRowLength = r.buffer_row_length; c.bufferImageHeight = r.buffer_image_height;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.level, r.layer, 1};
    c.imageExtent = {r.width, r.height, std::max(1u, r.depth)};
    regs.push_back(c);
  }
  ToGeneral(s, t);
  EndRendering(s);
  GlobalBarrier(s.cb);
  g_vk.f.vkCmdCopyBufferToImage(s.cb, sb, t->image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regs.size(), regs.data());
  GlobalBarrier(s.cb);
  sr_tex_upload_free(&up);
  g_stats.uploads++;
}

static void PrepareSampled(RS& s, TextureG* g) {
  TextureImpl* t = g->impl;
  if (!t) return;
  ToGeneral(s, t);
  if (!t->uploaded && !t->gpuWritten) UploadTexture(s, t, g);
  t->lastUse = s.value;
}

static void InitDummies(RS& s) {
  if (g_dummyInit) return;
  g_dummyInit = true;
  EndRendering(s);                    // the clears and layout changes below are not allowed inside a render pass
  auto& F = g_vk.f;
  struct D { int dim; VkImageType type; VkImageViewType vt; uint32_t layers; VkImageCreateFlags fl; } ds[7] = {
    {0, VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, 0}, {1, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0}, {2, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0},
    {3, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT}, {4, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 2, 0},
    {5, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT}, {6, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0}};
  for (auto& d : ds) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = d.fl; ci.imageType = d.type; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {1, 1, 1}; ci.mipLevels = 1; ci.arrayLayers = d.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Dummy& m = g_dummy[d.dim];
    F.vkCreateImage(g_vk.dev, &ci, nullptr, &m.img);
    VkMemoryRequirements rq; F.vkGetImageMemoryRequirements(g_vk.dev, m.img, &rq);
    AllocMemory(MemKind::Device, rq, &m.mem);
    F.vkBindImageMemory(g_vk.dev, m.img, m.mem.mem, m.mem.offset);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m.img; vi.viewType = d.vt; vi.format = ci.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers};
    F.vkCreateImageView(g_vk.dev, &vi, nullptr, &m.view);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    F.vkCmdPipelineBarrier2(s.cb, &di);
    VkClearColorValue white{{1, 1, 1, 1}}; VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers};
    F.vkCmdClearColorImage(s.cb, m.img, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &rg);
  }
  GlobalBarrier(s.cb);
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.maxLod = 1.0f;
  F.vkCreateSampler(g_vk.dev, &si, nullptr, &g_dummySampler);
}

// ---- pipeline building ---------------------------------------------------------------------------------------------
template <class T> static void Put(std::string& k, const T& v) { k.append((const char*)&v, sizeof v); }

static VkBlendFactor BF(uint32_t f) {
  switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
  }
  return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp BO(uint32_t e) {
  switch (e) { case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT; case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
               case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN; case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
static VkCompareOp CO(uint32_t f) {   // depth/stencil/alpha funcs share values 1..8
  static const VkCompareOp m[9] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
                                   VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
  return f <= 8 ? m[f] : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp SO(uint32_t o) {
  static const VkStencilOp m[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                   VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
  return o <= 8 ? m[o] : VK_STENCIL_OP_KEEP;
}
static bool Topology(uint32_t prim, VkPrimitiveTopology* t) {
  switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case NVN_DRAW_PRIMITIVE_LINES: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_PATCHES: *t = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; return true;
  }
  return false;   // LINE_LOOP, QUADS, QUAD_STRIP, POLYGON have no Vulkan equivalent
}

static VkFormat RtFormat(const RtBind& r) {
  TextureImpl* t = r.tex->impl;
  if (r.view.flags & 2) { int a, b; VkImageAspectFlags c; VkFormat f = VkFormatFromNvn(r.view.format, &a, &b, &c); if (f != VK_FORMAT_UNDEFINED) return f; }
  return t->vkFormat;
}

static VkPipeline BuildGraphics(RS& s, uint32_t prim) {
  auto& F = g_vk.f;
  ProgramImpl* p = s.prog;
  VkPrimitiveTopology topo;
  if (!Topology(prim, &topo)) { LogOnce("badprim", "draw primitive %u has no Vulkan topology (draw skipped)", prim); return VK_NULL_HANDLE; }
  StageProgram* vs = p->byStage[NVN_SHADER_STAGE_VERTEX];
  VkFormat cfmt[8]; for (uint32_t i = 0; i < s.nColor; i++) cfmt[i] = s.color[i].tex ? RtFormat(s.color[i]) : VK_FORMAT_UNDEFINED;
  VkFormat dfmt = s.depth.tex ? RtFormat(s.depth) : VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  { TextureG* any = s.nColor && s.color[0].tex ? s.color[0].tex : s.depth.tex; if (any) { uint32_t n = std::max(1u, any->samples); samples = (VkSampleCountFlagBits)n; } }

  std::string key;
  Put(key, p->id); Put(key, prim); Put(key, s.nColor); for (uint32_t i = 0; i < s.nColor; i++) Put(key, cfmt[i]); Put(key, dfmt); Put(key, samples);
  Put(key, s.colorState); Put(key, s.chan); Put(key, s.poly); Put(key, s.ds); Put(key, s.ms.a2c);
  for (uint32_t i = 0; i < s.nColor; i++) Put(key, s.blend[i]);
  Put(key, s.originMode); Put(key, s.depthClamp); Put(key, s.discard); Put(key, s.primRestart); Put(key, s.sampleMask); Put(key, s.patchSize); Put(key, s.depthMode); Put(key, s.depthBoundsOn);
  Put(key, s.polyFactor != 0 || s.polyUnits != 0);
  for (auto& in : vs->man.inputs) { Put(key, s.attr[in.attr & 15]); Put(key, s.attrSet[in.attr & 15]); Put(key, s.stream[s.attr[in.attr & 15].stream & 15]); }
  auto it = g_pipes.find(key);
  if (it != g_pipes.end()) return it->second;

  std::vector<VkPipelineShaderStageCreateInfo> stages;
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg[5];
  for (int st = 0; st < 5; st++) {
    StageProgram* sp = p->byStage[st];
    if (!sp) continue;
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    si.stage = StageBit(st); si.module = sp->module; si.pName = "main";
    if (WantSubgroup32(sp, si.stage)) { sg[st] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, nullptr, 32}; si.pNext = &sg[st]; }
    stages.push_back(si);
  }
  // vertex input
  std::vector<VkVertexInputBindingDescription> binds; std::vector<VkVertexInputAttributeDescription> attrs;
  bool used[17] = {};
  for (auto& in : vs->man.inputs) {
    uint32_t a = in.attr & 15; VkVertexInputAttributeDescription d{};
    d.location = in.location;
    // The shader declares the numeric class of the input (manifest type: 0 float, 1 int, 2 uint). A vertex format of another class
    // would make the pipeline invalid, so such an attribute reads the default value of the class the shader wants.
    const VkFormat defFmt = in.type == 2 ? VK_FORMAT_R32G32B32A32_UINT : in.type == 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    if (s.attrSet[a] && s.attr[a].format != 0) {
      bool isInt = false; d.format = VertexFormatFromNvn(s.attr[a].format, &isInt);
      d.offset = s.attr[a].offset; d.binding = s.attr[a].stream & 15;
      if (d.format == VK_FORMAT_UNDEFINED) { LogOnce("vtxfmt", "vertex format 0x%x not supported (attribute %u reads defaults)", s.attr[a].format, a); d.format = defFmt; d.binding = 16; d.offset = 0; }
      else if (isInt != (in.type != 0)) { LogOnce("vtxkind", "vertex attribute %u: format 0x%x and the shader input disagree on integer or float (attribute reads defaults)", a, s.attr[a].format); d.format = defFmt; d.binding = 16; d.offset = 0; }
    } else { d.format = defFmt; d.binding = 16; d.offset = 0; }
    used[d.binding] = true; attrs.push_back(d);
  }
  for (uint32_t b = 0; b < 17; b++) {
    if (!used[b]) continue;
    VkVertexInputBindingDescription d{}; d.binding = b;
    if (b == 16) { d.stride = 0; d.inputRate = VK_VERTEX_INPUT_RATE_VERTEX; }
    else { d.stride = s.stream[b].stride; d.inputRate = s.stream[b].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
           if (s.stream[b].divisor > 1) LogOnce("divisor", "vertex stream divisor %u > 1 is not supported (treated as 1)", s.stream[b].divisor); }
    binds.push_back(d);
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = (uint32_t)binds.size(); vi.pVertexBindingDescriptions = binds.data();
  vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size(); vi.pVertexAttributeDescriptions = attrs.data();
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topo; ia.primitiveRestartEnable = s.primRestart && (topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY);
  VkPipelineTessellationStateCreateInfo ti{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO}; ti.patchControlPoints = std::max(1u, s.patchSize);
  VkPipelineViewportDepthClipControlCreateInfoEXT dcc{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
  if (s.depthMode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W) {
    if (g_vk.hasDepthClipControl) { dcc.negativeOneToOne = VK_TRUE; vp.pNext = &dcc; }
    else LogOnce("nodcc", "depth mode NEAR_IS_MINUS_W needs VK_EXT_depth_clip_control (using zero-to-one)");
  }
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.depthClampEnable = s.depthClamp && g_vk.feat.features.depthClamp; rs.rasterizerDiscardEnable = s.discard;
  rs.polygonMode = s.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : s.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  rs.cullMode = s.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : s.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : s.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
  { bool cw = s.poly.front == NVN_FRONT_FACE_CW; if (s.originMode == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) cw = !cw;   // positive viewport height mirrors the winding
    rs.frontFace = cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE; }
  rs.depthBiasEnable = (s.polyFactor != 0 || s.polyUnits != 0) && s.poly.offsets != 0; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = samples; ms.alphaToCoverageEnable = s.ms.a2c; VkSampleMask mask = s.sampleMask; ms.pSampleMask = &mask;
  VkPipelineDepthStencilStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dsi.depthTestEnable = (s.ds.flags & 1) != 0; dsi.depthWriteEnable = (s.ds.flags & 2) != 0; dsi.depthCompareOp = CO(s.ds.depthFunc);
  dsi.stencilTestEnable = (s.ds.flags & 4) != 0 && dfmt != VK_FORMAT_UNDEFINED;
  dsi.front = {SO(s.ds.opF & 15), SO(s.ds.opF2 & 15), SO(s.ds.opF >> 4), CO(s.ds.func & 15), 0xFF, 0xFF, 0};
  dsi.back = {SO(s.ds.opB & 15), SO(s.ds.opB2 & 15), SO(s.ds.opB >> 4), CO(s.ds.func >> 4), 0xFF, 0xFF, 0};
  dsi.depthBoundsTestEnable = s.depthBoundsOn && g_vk.feat.features.depthBounds;
  VkPipelineColorBlendAttachmentState cba[8];
  for (uint32_t i = 0; i < s.nColor; i++) {
    const BlendStateG& b = s.blend[i]; auto& c = cba[i]; c = {};
    c.blendEnable = (s.colorState.blendMask >> i) & 1;
    c.srcColorBlendFactor = BF(b.srcRGB); c.dstColorBlendFactor = BF(b.dstRGB); c.colorBlendOp = BO(b.eq & 7);
    c.srcAlphaBlendFactor = BF(b.srcA); c.dstAlphaBlendFactor = BF(b.dstA); c.alphaBlendOp = BO((b.eq >> 3) & 7);
    uint32_t m = (s.chan.masks >> (4 * i)) & 15;
    c.colorWriteMask = ((m & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((m & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) | ((m & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((m & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = s.nColor; cb.pAttachments = cba;
  if (s.colorState.logicOp != NVN_LOGIC_OP_COPY && g_vk.feat.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)s.colorState.logicOp; }
  if (s.colorState.alphaFunc != NVN_ALPHA_FUNC_ALWAYS && s.colorState.alphaFunc != 0) LogOnce("alphatest", "fixed function alpha test (func %u) is not emulated", s.colorState.alphaFunc);
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                          VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BOUNDS};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 8; dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = s.nColor; ri.pColorAttachmentFormats = cfmt;
  if (dfmt != VK_FORMAT_UNDEFINED) { VkFormat d = dfmt; if (d != VK_FORMAT_S8_UINT) ri.depthAttachmentFormat = d;
                                      if (d == VK_FORMAT_S8_UINT || d == VK_FORMAT_D24_UNORM_S8_UINT || d == VK_FORMAT_D32_SFLOAT_S8_UINT) ri.stencilAttachmentFormat = d; }
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &ri; gi.stageCount = (uint32_t)stages.size(); gi.pStages = stages.data(); gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
  if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) gi.pTessellationState = &ti;
  gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &dsi; gi.pColorBlendState = &cb; gi.pDynamicState = &dy;
  gi.layout = GetLayout(p);
  VkPipeline out = VK_NULL_HANDLE;
  VkResult r = F.vkCreateGraphicsPipelines(g_vk.dev, g_pcache, 1, &gi, nullptr, &out);
  if (r != VK_SUCCESS) { Err("vkCreateGraphicsPipelines failed (%d) program %llu", r, (unsigned long long)p->id); out = VK_NULL_HANDLE; g_stats.pipelineFails++; }
  g_pipes[key] = out; g_stats.pipelines++;
  return out;
}

static VkPipeline BuildCompute(RS& s) {
  ProgramImpl* p = s.prog; StageProgram* cs = p->byStage[NVN_SHADER_STAGE_COMPUTE];
  std::string key; Put(key, p->id); Put(key, 'C');
  auto it = g_pipes.find(key);
  if (it != g_pipes.end()) return it->second;
  VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
  uint32_t ls[3] = {cs->man.localSize[0], cs->man.localSize[1], cs->man.localSize[2]};
  VkSpecializationInfo sp{3, me, 12, ls};
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, nullptr, 32};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, WantSubgroup32(cs, VK_SHADER_STAGE_COMPUTE_BIT) ? &sg : nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs->module, "main", &sp};
  ci.layout = GetLayout(p);
  VkPipeline out = VK_NULL_HANDLE;
  if (g_vk.f.vkCreateComputePipelines(g_vk.dev, g_pcache, 1, &ci, nullptr, &out) != VK_SUCCESS) out = VK_NULL_HANDLE;
  g_pipes[key] = out; g_stats.pipelines++;
  return out;
}

// ---- descriptors for a draw/dispatch ----------------------------------------------------------------------------------
// The Ryujinx style support buffer (docs/SHADER_CACHE.md section 5): zeros except the viewport inverse and size vectors and a
// render scale of 1.0 in every scale slot. Alpha test 0 (always pass) and is_bgra 0 (RGBA targets) are the zero defaults.
static void FillSupportBuffer(const RS& s, uint8_t* p, uint32_t size) {
  memset(p, 0, size);
  float w = s.vpSet ? (float)s.viewport[2] : (float)s.area.width, h = s.vpSet ? (float)s.viewport[3] : (float)s.area.height;
  float inv[4] = {w > 0 ? 2.0f / w : 0.0f, h > 0 ? 2.0f / h : 0.0f, 1.0f, 1.0f}, sz[4] = {w, h, 1.0f, 1.0f};
  if (size >= 176) { memcpy(p + 144, inv, 16); memcpy(p + 160, sz, 16); }
  const float one = 1.0f;
  for (uint32_t i = 0; i < 73 && 192 + 16 * i + 4 <= size; i++) memcpy(p + 192 + 16 * i, &one, 4);
}

static bool BindDescriptors(RS& s, bool compute) {
  auto& F = g_vk.f;
  ProgramImpl* p = s.prog;
  InitDummies(s);
  VkPipelineLayout pl = GetLayout(p);
  VkPipelineBindPoint bp = compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
  for (int st = 0; st < 6; st++) {
    StageProgram* sp = p->byStage[st];
    if (!sp) continue;
    const Manifest& m = sp->man;
    VkDescriptorSet set = s.dpools->Alloc(sp->setLayout);
    if (!set) { Err("descriptor set allocation failed"); return false; }
    std::vector<VkWriteDescriptorSet> w;
    std::vector<VkDescriptorBufferInfo> bi; bi.reserve(m.ubos.size() + m.ssbos.size() + 1);
    std::vector<VkDescriptorImageInfo> ii; size_t nimg = m.imgs.size(); for (auto& t : m.texs) nimg += std::max(1u, t.arrayLen); ii.reserve(nimg);
    if (m.supportBinding >= 0) {      // translator support buffer: viewport and render scale words (SHADER_CACHE.md section 5)
      VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize so = 0; void* ptr = nullptr;
      VkDeviceSize size = std::max<VkDeviceSize>(m.supportSize, 16);
      if (s.scratch->Alloc((size + 15) & ~15ull, std::max<VkDeviceSize>(256, g_vk.props.limits.minUniformBufferOffsetAlignment), &buf, &so, &ptr)) {
        FillSupportBuffer(s, (uint8_t*)ptr, (uint32_t)size);
      } else { buf = g_nullVb; so = 0; size = 256; }
      bi.push_back({buf, so, size});
      VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = set; x.dstBinding = (uint32_t)m.supportBinding; x.descriptorCount = 1;
      x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; x.pBufferInfo = &bi.back(); w.push_back(x);
    }
    for (auto& u : m.ubos) {
      VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0, range = std::max<uint32_t>(u.size, 16);
      int src = u.source;
      if (src == UBO_NVN && u.nvnIndex < 0) src = u.cbuf == 2 ? UBO_HANDLES_C2 : u.cbuf == 0 ? UBO_DRIVER_C0 : u.cbuf == 1 ? UBO_PROGRAM_CONSTANTS : UBO_NVN;   // v1 manifest
      if (src == UBO_NVN) {
        if (u.nvnIndex >= 0 && u.nvnIndex < 16 && s.ub[st][u.nvnIndex].addr) {
          const auto& r = s.ub[st][u.nvnIndex];
          uint64_t o;
          if (ResolveBuffer(r.addr, 1, &buf, &o)) { off = o; if (r.size) range = r.size; }
          else buf = VK_NULL_HANDLE;
        }
      } else if (src == UBO_PROGRAM_CONSTANTS) {
        if (ShaderCacheConstants(sp, &buf, &off)) range = 64 * 1024;
      } else {
        void* ptr; VkDeviceSize so;
        range = (range + 15) & ~15ull;
        if (s.scratch->Alloc(range, std::max<VkDeviceSize>(256, g_vk.props.limits.minUniformBufferOffsetAlignment), &buf, &so, &ptr)) {
          memset(ptr, 0, range); off = so;
          if (src == UBO_HANDLES_C2) memcpy(ptr, s.c2[st], std::min<size_t>(range, sizeof s.c2[st]));
          else for (int i = 0; i < 16; i++) { if (0x110 + 16 * (size_t)i + 16 > range) break; uint8_t* d = (uint8_t*)ptr + 0x110 + 16 * i; memcpy(d, &s.sb[st][i].addr, 8); uint32_t sz = (uint32_t)s.sb[st][i].size; memcpy(d + 8, &sz, 4); }
        }
      }
      range = std::min<VkDeviceSize>(range, g_vk.props.limits.maxUniformBufferRange);
      if (!buf) { buf = g_nullVb; off = 0; range = 256; LogOnce(("ub" + sp->key + std::to_string(u.binding)).c_str(), "stage %s uniform binding %u (nvn index %d) is not bound", StageName(st), u.binding, u.nvnIndex); }
      bi.push_back({buf, off, range});
      VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = set; x.dstBinding = u.binding; x.descriptorCount = 1;
      x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; x.pBufferInfo = &bi.back(); w.push_back(x);
    }
    for (auto& t : m.texs) {
      uint32_t n = std::max(1u, t.arrayLen);
      size_t first = ii.size();
      for (uint32_t k = 0; k < n; k++) {
        uint32_t tw = t.hCbuf == 2 ? s.c2[st][std::min<uint32_t>(0x3FF, (t.hOff + 8 * k) / 4)] : 0;
        uint32_t sw = t.hasSampler && t.sCbuf == 2 ? s.c2[st][std::min<uint32_t>(0x3FF, (t.sOff + 8 * k) / 4)] : 0;
        uint32_t tid = tw & 0xFFFFF, sid = t.hasSampler ? (sw & 0xFFFFF) : (tw >> 20); uint32_t h = tw | sw;
        VkDescriptorImageInfo di{g_dummySampler, g_dummy[std::min(t.dim, 5)].view, VK_IMAGE_LAYOUT_GENERAL};
        if (t.res == RES_SAMPLER) { tid = UINT32_MAX; sid = tw & 0xFFFFF; }      // a pure sampler word carries only the sampler id
        if (s.texPool && tid < s.texPool->ents.size() && s.texPool->ents[tid].tex) {
          auto& e = s.texPool->ents[tid];
          PrepareSampled(s, e.texG);
          VkImageView v = GetView(e.tex, e.texG, e.hasView ? &e.view : nullptr, false);
          if (v) di.imageView = v;
        } else if (t.res != RES_SAMPLER) LogOnce(("tex" + std::to_string(tid)).c_str(), "draw samples texture id %u that is not registered in the bound texture pool", tid);
        if (s.sampPool && sid < s.sampPool->ents.size() && s.sampPool->ents[sid] && s.sampPool->ents[sid]->sampler) di.sampler = s.sampPool->ents[sid]->sampler;
        else if (sid || h) LogOnce(("smp" + std::to_string(sid)).c_str(), "draw uses sampler id %u that is not registered", sid);
        if (t.res == RES_SAMPLED_IMAGE) di.sampler = VK_NULL_HANDLE;
        if (t.res == RES_SAMPLER) di.imageView = VK_NULL_HANDLE;
        ii.push_back(di);
      }
      VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = set; x.dstBinding = t.binding; x.descriptorCount = n;
      x.descriptorType = TexDescType(t); x.pImageInfo = &ii[first]; w.push_back(x);
    }
    for (auto& im : m.imgs) {
      VkDescriptorImageInfo di{VK_NULL_HANDLE, g_dummy[1].view, VK_IMAGE_LAYOUT_GENERAL};
      uint64_t h = (im.index >= 0 && im.index < 32) ? s.img[st][im.index] : 0;
      uint32_t tid = (uint32_t)h & 0xFFFFF;
      if (h && s.texPool && tid < s.texPool->ents.size() && s.texPool->ents[tid].tex) {
        auto& e = s.texPool->ents[tid]; PrepareSampled(s, e.texG);
        TextureImpl* ti = e.tex; ti->gpuWritten = true;
        VkImageView v = GetView(ti, e.texG, e.hasView ? &e.view : nullptr, false); if (v) di.imageView = v;
      }
      ii.push_back(di);
      VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = set; x.dstBinding = im.binding; x.descriptorCount = 1;
      x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; x.pImageInfo = &ii.back(); w.push_back(x);
    }
    for (auto& sb : m.ssbos) {
      VkBuffer buf = VK_NULL_HANDLE; uint64_t o = 0; VkDeviceSize range = 256;
      if (sb.nvnIndex >= 0 && sb.nvnIndex < 16 && s.sb[st][sb.nvnIndex].addr && ResolveBuffer(s.sb[st][sb.nvnIndex].addr, 1, &buf, &o)) range = s.sb[st][sb.nvnIndex].size ? s.sb[st][sb.nvnIndex].size : VK_WHOLE_SIZE;
      else { buf = g_nullVb; o = 0; LogOnce(("sb" + sp->key).c_str(), "stage %s storage buffer %d is not bound", StageName(st), sb.nvnIndex); }
      bi.push_back({buf, o, range});
      VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = set; x.dstBinding = sb.binding; x.descriptorCount = 1;
      x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x.pBufferInfo = &bi.back(); w.push_back(x);
    }
    if (!w.empty()) F.vkUpdateDescriptorSets(g_vk.dev, (uint32_t)w.size(), w.data(), 0, nullptr);
    F.vkCmdBindDescriptorSets(s.cb, bp, pl, SetOfStage(st), 1, &set, 0, nullptr);
  }
  return true;
}

// ---- render pass handling ---------------------------------------------------------------------------------------------
static bool BeginRendering(RS& s) {
  auto& F = g_vk.f;
  if (s.rendering && !s.rtDirty) return true;
  EndRendering(s);
  if (!s.nColor && !s.depth.tex) { LogOnce("nort", "draw with no render targets bound (skipped)"); return false; }
  VkRenderingAttachmentInfo ca[8]; VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = 1; bool haveDepth = false, haveStencil = false;
  for (uint32_t i = 0; i < s.nColor; i++) {
    TextureG* g = s.color[i].tex; TextureImpl* t = g->impl;
    ToGeneral(s, t); t->gpuWritten = true; t->lastUse = s.value;
    VkImageView v = GetView(t, g, &s.color[i].view, true);
    ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca[i].imageView = v; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    uint32_t lv = (s.color[i].view.flags & 8) ? s.color[i].view.minLevel : 0;
    w = std::min(w, std::max(1u, t->width >> lv)); h = std::min(h, std::max(1u, t->height >> lv));
    layers = (s.color[i].view.flags & 16) ? std::max<uint32_t>(1, s.color[i].view.numLayers) : 1;
  }
  if (s.depth.tex) {
    TextureG* g = s.depth.tex; TextureImpl* t = g->impl;
    ToGeneral(s, t); t->gpuWritten = true; t->lastUse = s.value;
    da.imageView = GetView(t, g, &s.depth.view, true); da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    uint32_t lv = (s.depth.view.flags & 8) ? s.depth.view.minLevel : 0;
    w = std::min(w, std::max(1u, t->width >> lv)); h = std::min(h, std::max(1u, t->height >> lv));
    haveDepth = (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; haveStencil = (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
  }
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = layers; ri.colorAttachmentCount = s.nColor; ri.pColorAttachments = ca;
  if (haveDepth) ri.pDepthAttachment = &da;
  if (haveStencil) ri.pStencilAttachment = &da;
  F.vkCmdBeginRendering(s.cb, &ri);
  s.rendering = true; s.rtDirty = false; s.area = {w, h}; s.renderLayers = layers; s.pipeDirty = true;
  return true;
}

static void SetDynamic(RS& s) {
  auto& F = g_vk.f;
  VkViewport v;
  int32_t x = s.vpSet ? s.viewport[0] : 0, y = s.vpSet ? s.viewport[1] : 0, vw = s.vpSet ? s.viewport[2] : (int32_t)s.area.width, vh = s.vpSet ? s.viewport[3] : (int32_t)s.area.height;
  v.x = (float)x; v.minDepth = s.depthN; v.maxDepth = s.depthF;
  if (s.originMode == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) { v.y = (float)y; v.height = (float)vh; }   // row 0 is the bottom: same numbers as GL
  else { v.y = (float)(y + vh); v.height = -(float)vh; }                                              // row 0 is the top, NDC +y up
  v.width = (float)vw;
  F.vkCmdSetViewport(s.cb, 0, 1, &v);
  VkRect2D sc;
  if (s.scSet) sc = {{s.scissor[0], s.scissor[1]}, {(uint32_t)std::max(0, s.scissor[2]), (uint32_t)std::max(0, s.scissor[3])}};
  else sc = {{0, 0}, s.area};
  if (sc.offset.x < 0) { sc.extent.width = (uint32_t)std::max<int64_t>(0, (int64_t)sc.extent.width + sc.offset.x); sc.offset.x = 0; }
  if (sc.offset.y < 0) { sc.extent.height = (uint32_t)std::max<int64_t>(0, (int64_t)sc.extent.height + sc.offset.y); sc.offset.y = 0; }
  F.vkCmdSetScissor(s.cb, 0, 1, &sc);
  F.vkCmdSetStencilReference(s.cb, VK_STENCIL_FACE_FRONT_BIT, s.stRef[0]); F.vkCmdSetStencilReference(s.cb, VK_STENCIL_FACE_BACK_BIT, s.stRef[1]);
  F.vkCmdSetStencilCompareMask(s.cb, VK_STENCIL_FACE_FRONT_BIT, s.stMask[0]); F.vkCmdSetStencilCompareMask(s.cb, VK_STENCIL_FACE_BACK_BIT, s.stMask[1]);
  F.vkCmdSetStencilWriteMask(s.cb, VK_STENCIL_FACE_FRONT_BIT, s.stWrite[0]); F.vkCmdSetStencilWriteMask(s.cb, VK_STENCIL_FACE_BACK_BIT, s.stWrite[1]);
  F.vkCmdSetDepthBias(s.cb, s.polyUnits, s.polyClamp, s.polyFactor);
  F.vkCmdSetLineWidth(s.cb, g_vk.feat.features.wideLines ? s.lineWidth : 1.0f);
  if (g_vk.feat.features.depthBounds) F.vkCmdSetDepthBounds(s.cb, s.depthBounds[0], s.depthBounds[1]);
}

// Layout transitions, uploads and the dummy images are recorded outside a render pass, so every texture the draw can sample is
// made ready here, before BeginRendering. BindDescriptors then finds them ready.
static void PrepareResources(RS& s) {
  InitDummies(s);
  for (int st = 0; st < 6; st++) {
    StageProgram* sp = s.prog->byStage[st];
    if (!sp || !s.texPool) continue;
    for (auto& t : sp->man.texs) {
      uint32_t n = std::max(1u, t.arrayLen);
      for (uint32_t k = 0; k < n; k++) {
        uint32_t tw = t.hCbuf == 2 ? s.c2[st][std::min<uint32_t>(0x3FF, (t.hOff + 8 * k) / 4)] : 0;
        uint32_t tid = tw & 0xFFFFF;
        if (tid < s.texPool->ents.size() && s.texPool->ents[tid].tex) PrepareSampled(s, s.texPool->ents[tid].texG);
      }
    }
    for (auto& im : sp->man.imgs) {
      uint64_t h = (im.index >= 0 && im.index < 32) ? s.img[st][im.index] : 0;
      uint32_t tid = (uint32_t)h & 0xFFFFF;
      if (h && tid < s.texPool->ents.size() && s.texPool->ents[tid].tex) PrepareSampled(s, s.texPool->ents[tid].texG);
    }
  }
}

static bool PrepareDraw(RS& s, uint32_t prim, bool* skip) {
  *skip = true;
  if (!s.prog || !s.prog->complete) { LogOnce("noprog", "draw without a complete program (skipped)"); return false; }
  for (int st = 0; st < 5; st++) if (s.prog->byStage[st] && !EnsureStage(s.prog->byStage[st])) { return false; }
  EnsureGlobals();
  PrepareResources(s);
  if (!BeginRendering(s)) return false;
  VkPipeline pipe = BuildGraphics(s, prim);
  if (!pipe) return false;
  g_vk.f.vkCmdBindPipeline(s.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  SetDynamic(s);
  if (!BindDescriptors(s, false)) return false;
  // vertex buffers
  uint32_t maxb = 0; bool any = false;
  VkBuffer vbs[17]; VkDeviceSize offs[17];
  for (uint32_t b = 0; b < 16; b++) {
    vbs[b] = g_nullVb; offs[b] = 0;
    if (s.vb[b].addr) { VkBuffer buf; uint64_t o; if (ResolveBuffer(s.vb[b].addr, 1, &buf, &o)) { vbs[b] = buf; offs[b] = o; maxb = b + 1; any = true; } }
  }
  vbs[16] = g_nullVb; offs[16] = 0;
  g_vk.f.vkCmdBindVertexBuffers(s.cb, 0, 17, vbs, offs);
  (void)any; (void)maxb;
  *skip = false;
  return true;
}

// ---- exec ---------------------------------------------------------------------------------------------------------------
static void Exec(RS& s, const CmdList& l, int depth);

static TextureView ViewAt(const uint8_t* p) { TextureView v; memcpy(&v, p, sizeof v); return v; }

static void DoClearColor(RS& s, const Cmd& c) {
  uint32_t idx = (uint32_t)c.i[0];
  if (idx >= s.nColor || !s.color[idx].tex) { LogOnce("clr", "ClearColor of target %u that is not bound", idx); return; }
  if (!BeginRendering(s)) return;
  VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = idx; memcpy(&ca.clearValue.color, c.st, 16);
  if ((uint32_t)c.i[1] != 0xF) LogOnce("clrmask", "ClearColor with a partial color mask clears all channels");
  VkClearRect cr{}; cr.rect = {{0, 0}, s.area}; cr.layerCount = s.renderLayers;
  if (s.scSet) { cr.rect.offset = {std::max(0, s.scissor[0]), std::max(0, s.scissor[1])}; cr.rect.extent = {(uint32_t)std::max(0, s.scissor[2]), (uint32_t)std::max(0, s.scissor[3])}; }
  g_vk.f.vkCmdClearAttachments(s.cb, 1, &ca, 1, &cr); g_stats.clears++;
}

static void DoClearDepth(RS& s, const Cmd& c) {
  if (!s.depth.tex) { LogOnce("clrd", "ClearDepthStencil with no depth target bound"); return; }
  if (!BeginRendering(s)) return;
  VkImageAspectFlags a = 0; TextureImpl* t = s.depth.tex->impl;
  if (c.i[0] && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
  if (c.i[2] && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
  if (!a) return;
  VkClearAttachment ca{}; ca.aspectMask = a; ca.clearValue.depthStencil = {c.f[0], (uint32_t)c.i[1]};
  VkClearRect cr{}; cr.rect = {{0, 0}, s.area}; cr.layerCount = s.renderLayers;
  if (s.scSet) { cr.rect.offset = {std::max(0, s.scissor[0]), std::max(0, s.scissor[1])}; cr.rect.extent = {(uint32_t)std::max(0, s.scissor[2]), (uint32_t)std::max(0, s.scissor[3])}; }
  g_vk.f.vkCmdClearAttachments(s.cb, 1, &ca, 1, &cr); g_stats.clears++;
}

static VkImageSubresourceLayers SubLayers(TextureImpl* t, const TextureView& v, bool hasV) {
  uint32_t lv = hasV && (v.flags & 8) ? v.minLevel : 0, ly = hasV && (v.flags & 16) ? v.minLayer : 0;
  uint32_t cnt = hasV && (v.flags & 16) ? std::max<uint32_t>(1, v.numLayers) : 1;
  return {t->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect) : t->aspect, lv, ly, cnt};
}

static void DoCopy(RS& s, const Cmd& c) {
  auto& F = g_vk.f;
  EndRendering(s); GlobalBarrier(s.cb);
  switch (c.op) {
    case Op::CopyBufToBuf: {
      VkBuffer a, b; uint64_t oa, ob;
      if (!ResolveBuffer((uint64_t)c.l[0], (uint64_t)c.l[2], &a, &oa) || !ResolveBuffer((uint64_t)c.l[1], (uint64_t)c.l[2], &b, &ob)) { LogOnce("cpb", "CopyBufferToBuffer with unresolved address"); break; }
      VkBufferCopy r{oa, ob, (VkDeviceSize)c.l[2]}; F.vkCmdCopyBuffer(s.cb, a, b, 1, &r); break;
    }
    case Op::CopyBufToTex: {
      TextureG* g = (TextureG*)c.obj[0]; TextureImpl* t = g->impl; TextureView v = ViewAt(c.blob.data()); const int32_t* rg = (const int32_t*)(c.blob.data() + 16);
      VkBuffer b; uint64_t o; if (!t || !ResolveBuffer((uint64_t)c.l[0], 1, &b, &o)) { LogOnce("cpbt", "CopyBufferToTexture with unresolved address"); break; }
      ToGeneral(s, t); t->gpuWritten = true; t->uploaded = true; t->lastUse = s.value;
      VkBufferImageCopy r{}; r.bufferOffset = o; r.imageSubresource = SubLayers(t, v, v.flags != 0);
      r.imageOffset = {rg[0], rg[1], rg[2]}; r.imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], (uint32_t)std::max(1, rg[5])};
      if (!t->is3D) { r.imageOffset.z = 0; r.imageExtent.depth = 1; r.imageSubresource.baseArrayLayer += (uint32_t)rg[2]; r.imageSubresource.layerCount = std::max(1, rg[5]); }
      F.vkCmdCopyBufferToImage(s.cb, b, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r); break;
    }
    case Op::CopyTexToBuf: {
      TextureG* g = (TextureG*)c.obj[0]; TextureImpl* t = g->impl; TextureView v = ViewAt(c.blob.data()); const int32_t* rg = (const int32_t*)(c.blob.data() + 16);
      VkBuffer b; uint64_t o; if (!t || !ResolveBuffer((uint64_t)c.l[0], 1, &b, &o)) { LogOnce("cptb", "CopyTextureToBuffer with unresolved address"); break; }
      ToGeneral(s, t); PrepareSampled(s, g);
      VkBufferImageCopy r{}; r.bufferOffset = o; r.imageSubresource = SubLayers(t, v, v.flags != 0);
      r.imageOffset = {rg[0], rg[1], rg[2]}; r.imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], (uint32_t)std::max(1, rg[5])};
      if (!t->is3D) { r.imageOffset.z = 0; r.imageExtent.depth = 1; r.imageSubresource.baseArrayLayer += (uint32_t)rg[2]; r.imageSubresource.layerCount = std::max(1, rg[5]); }
      F.vkCmdCopyImageToBuffer(s.cb, t->image, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r); break;
    }
    case Op::CopyTexToTex: {
      TextureG* sg = (TextureG*)c.obj[0]; TextureG* dg = (TextureG*)c.obj[1]; TextureImpl* st = sg->impl; TextureImpl* dt = dg->impl;
      TextureView sv = ViewAt(c.blob.data()); const int32_t* sr = (const int32_t*)(c.blob.data() + 16);
      TextureView dv = ViewAt(c.blob.data() + 40); const int32_t* dr = (const int32_t*)(c.blob.data() + 56);
      if (!st || !dt) break;
      PrepareSampled(s, sg); ToGeneral(s, dt); dt->gpuWritten = true; dt->uploaded = true; dt->lastUse = s.value;
      bool same = (sr[3] == dr[3] && sr[4] == dr[4]);
      VkImageSubresourceLayers a = SubLayers(st, sv, sv.flags != 0), b = SubLayers(dt, dv, dv.flags != 0);
      if (same && st->samples == dt->samples) {
        VkImageCopy r{}; r.srcSubresource = a; r.dstSubresource = b; r.srcOffset = {sr[0], sr[1], st->is3D ? sr[2] : 0}; r.dstOffset = {dr[0], dr[1], dt->is3D ? dr[2] : 0};
        r.extent = {(uint32_t)sr[3], (uint32_t)sr[4], (uint32_t)std::max(1, sr[5])}; if (!st->is3D) r.extent.depth = 1;
        F.vkCmdCopyImage(s.cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
      } else {
        VkImageBlit r{}; r.srcSubresource = a; r.dstSubresource = b;
        r.srcOffsets[0] = {sr[0], sr[1], 0}; r.srcOffsets[1] = {sr[0] + sr[3], sr[1] + sr[4], 1};
        r.dstOffsets[0] = {dr[0], dr[1], 0}; r.dstOffsets[1] = {dr[0] + dr[3], dr[1] + dr[4], 1};
        F.vkCmdBlitImage(s.cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, (c.i[0] & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
      }
      break;
    }
    default: break;
  }
  GlobalBarrier(s.cb); g_stats.copies++;
}

static void Exec(RS& s, const CmdList& l, int depth) {
  auto& F = g_vk.f;
  for (const Cmd& c : l.cmds) {
    switch (c.op) {
      case Op::BindBlend: { BlendStateG b; memcpy(&b, c.st, sizeof b); if (b.target < 8) s.blend[b.target] = b; break; }
      case Op::BindColor: memcpy(&s.colorState, c.st, sizeof s.colorState); break;
      case Op::BindChannelMask: memcpy(&s.chan, c.st, sizeof s.chan); break;
      case Op::BindDepthStencil: memcpy(&s.ds, c.st, sizeof s.ds); break;
      case Op::BindMultisample: memcpy(&s.ms, c.st, sizeof s.ms); break;
      case Op::BindPolygon: memcpy(&s.poly, c.st, sizeof s.poly); break;
      case Op::BindVertexAttrib: if (c.i[1]) for (auto& b : s.attrSet) b = false;
        if (c.i[0] >= 0 && c.i[0] < 16) { memcpy(&s.attr[c.i[0]], c.st, sizeof(VertexAttribG)); s.attrSet[c.i[0]] = true; } break;
      case Op::BindVertexStream: if (c.i[0] >= 0 && c.i[0] < 16) memcpy(&s.stream[c.i[0]], c.st, sizeof(VertexStreamG)); break;
      case Op::BindProgram: s.prog = (ProgramImpl*)c.p[0]; s.progStages = (uint32_t)c.i[0]; break;
      case Op::BindVertexBuffer: if (c.i[0] >= 0 && c.i[0] < 16) s.vb[c.i[0]] = {(uint64_t)c.l[0], (uint64_t)c.l[1]}; break;
      case Op::BindVertexBuffers: for (int k = 0; k < c.i[1]; k++) if (c.i[0] + k < 16) memcpy(&s.vb[c.i[0] + k], c.blob.data() + 16 * k, 16); break;
      case Op::BindUniform: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 16) s.ub[c.i[0]][c.i[1]] = {(uint64_t)c.l[0], (uint64_t)c.l[1]}; break;
      case Op::BindUniforms: for (int k = 0; k < c.i[2]; k++) if (c.i[0] < 6 && c.i[1] + k < 16) memcpy(&s.ub[c.i[0]][c.i[1] + k], c.blob.data() + 16 * k, 16); break;
      case Op::BindStorage: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 16) s.sb[c.i[0]][c.i[1]] = {(uint64_t)c.l[0], (uint64_t)c.l[1]}; break;
      case Op::BindSepTexture: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 64) s.c2[c.i[0]][(c.i[2] + 8 * c.i[1]) / 4] = (uint32_t)c.l[0]; break;
      case Op::BindSepSampler: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 64) s.c2[c.i[0]][(0x568 + 8 * c.i[1]) / 4] = (uint32_t)c.l[0]; break;
      case Op::BindImage: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 32) s.img[c.i[0]][c.i[1]] = (uint64_t)c.l[0]; break;
      case Op::SetRenderTargets: {
        uint32_t n = std::min(8, c.i[0]); s.nColor = n;
        const uint8_t* b = c.blob.data();
        for (uint32_t i = 0; i < 8; i++) {
          TextureG* t = nullptr; memcpy(&t, b + 8 * i, 8);
          s.color[i].tex = i < n ? t : nullptr;
          s.color[i].view = ViewAt(b + 64 + 16 * i);
        }
        memcpy(&s.depth.tex, b + 64 + 128, 8); s.depth.view = ViewAt(b + 64 + 128 + 8);
        s.rtDirty = true; s.pipeDirty = true; EndRendering(s); break;
      }
      case Op::SetViewport: memcpy(s.viewport, c.i, 16); s.vpSet = true; break;
      case Op::SetScissor: memcpy(s.scissor, c.i, 16); s.scSet = true; break;
      case Op::SetDepthRange: s.depthN = c.f[0]; s.depthF = c.f[1]; break;
      case Op::SetDepthBounds: s.depthBoundsOn = c.i[0] != 0; s.depthBounds[0] = c.f[0]; s.depthBounds[1] = c.f[1]; break;
      case Op::SetDepthClamp: s.depthClamp = c.i[0] != 0; break;
      case Op::SetLineWidth: s.lineWidth = c.f[0]; break;
      case Op::SetPointSize: break;
      case Op::SetPatchSize: s.patchSize = (uint32_t)c.i[0]; break;
      case Op::SetPolyOffset: s.polyFactor = c.f[0]; s.polyUnits = c.f[1]; s.polyClamp = c.f[2]; break;
      case Op::SetPrimRestart: s.primRestart = c.i[0] != 0; break;
      case Op::SetRasterDiscard: s.discard = c.i[0] != 0; break;
      case Op::SetSampleMask: s.sampleMask = (uint32_t)c.i[0]; break;
      case Op::SetStencilRef: if (c.i[0] & 1) s.stRef[0] = (uint32_t)c.i[1]; if (c.i[0] & 2) s.stRef[1] = (uint32_t)c.i[1]; break;
      case Op::SetStencilMask: if (c.i[0] & 1) s.stWrite[0] = (uint32_t)c.i[1]; if (c.i[0] & 2) s.stWrite[1] = (uint32_t)c.i[1]; break;
      case Op::SetStencilValueMask: if (c.i[0] & 1) s.stMask[0] = (uint32_t)c.i[1]; if (c.i[0] & 2) s.stMask[1] = (uint32_t)c.i[1]; break;
      case Op::SetTexturePool: s.texPool = (TexPool*)c.p[0]; break;
      case Op::SetSamplerPool: s.sampPool = (SampPool*)c.p[0]; break;
      case Op::Draw: case Op::DrawInstanced: {
        bool skip; if (!PrepareDraw(s, (uint32_t)c.i[0], &skip)) { g_stats.skippedDraws++; break; }
        F.vkCmdDraw(s.cb, (uint32_t)c.i[2], c.op == Op::Draw ? 1u : (uint32_t)c.i[4], (uint32_t)c.i[1], c.op == Op::Draw ? 0u : (uint32_t)c.i[3]); g_stats.draws++; break;
      }
      case Op::DrawElements: case Op::DrawElementsInstanced: {
        bool skip; if (!PrepareDraw(s, (uint32_t)c.i[0], &skip)) { g_stats.skippedDraws++; break; }
        VkBuffer ib; uint64_t io;
        if (!ResolveBuffer((uint64_t)c.l[0], 1, &ib, &io)) { LogOnce("badidx", "DrawElements index address unresolved (skipped)"); g_stats.skippedDraws++; break; }
        VkIndexType it = c.i[1] == NVN_INDEX_TYPE_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : c.i[1] == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT8;
        if (it == VK_INDEX_TYPE_UINT8 && !g_vk.hasIdx8) { LogOnce("idx8", "8-bit indices need VK_EXT_index_type_uint8"); g_stats.skippedDraws++; break; }
        F.vkCmdBindIndexBuffer(s.cb, ib, io, it);
        F.vkCmdDrawIndexed(s.cb, (uint32_t)c.i[2], c.op == Op::DrawElements ? 1u : (uint32_t)c.i[5], 0, c.i[3], c.op == Op::DrawElements ? 0u : (uint32_t)c.i[4]); g_stats.draws++; break;
      }
      case Op::DrawIndirect: case Op::DrawElementsIndirect: {
        bool skip; if (!PrepareDraw(s, (uint32_t)c.i[0], &skip)) { g_stats.skippedDraws++; break; }
        VkBuffer ib, db; uint64_t io, dof;
        if (!ResolveBuffer((uint64_t)c.l[1], 16, &db, &dof)) { g_stats.skippedDraws++; break; }
        if (c.op == Op::DrawElementsIndirect) {
          if (!ResolveBuffer((uint64_t)c.l[0], 1, &ib, &io)) { g_stats.skippedDraws++; break; }
          F.vkCmdBindIndexBuffer(s.cb, ib, io, c.i[1] == NVN_INDEX_TYPE_UNSIGNED_INT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
          F.vkCmdDrawIndexedIndirect(s.cb, db, dof, 1, 20);
        } else F.vkCmdDrawIndirect(s.cb, db, (uint64_t)dof, 1, 16);
        g_stats.draws++; break;
      }
      case Op::Dispatch: case Op::DispatchIndirect: {
        EndRendering(s);
        if (!s.prog || !s.prog->complete || !s.prog->byStage[NVN_SHADER_STAGE_COMPUTE]) { LogOnce("nocs", "dispatch without a compute program"); break; }
        if (!EnsureStage(s.prog->byStage[NVN_SHADER_STAGE_COMPUTE])) break;
        EnsureGlobals(); VkPipeline pipe = BuildCompute(s); if (!pipe) break;
        GlobalBarrier(s.cb);
        F.vkCmdBindPipeline(s.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        if (!BindDescriptors(s, true)) break;
        if (c.op == Op::Dispatch) F.vkCmdDispatch(s.cb, (uint32_t)c.i[0], (uint32_t)c.i[1], (uint32_t)c.i[2]);
        else { VkBuffer b; uint64_t o; if (ResolveBuffer((uint64_t)c.l[0], 12, &b, &o)) F.vkCmdDispatchIndirect(s.cb, b, o); }
        GlobalBarrier(s.cb); g_stats.dispatches++; break;
      }
      case Op::ClearColor: DoClearColor(s, c); break;
      case Op::ClearDepthStencil: DoClearDepth(s, c); break;
      case Op::CopyBufToBuf: case Op::CopyBufToTex: case Op::CopyTexToBuf: case Op::CopyTexToTex: DoCopy(s, c); break;
      case Op::Downsample: {
        TextureG* sg = (TextureG*)c.obj[0]; TextureG* dg = (TextureG*)c.obj[1]; if (!sg || !dg || !sg->impl || !dg->impl) break;
        EndRendering(s); ToGeneral(s, sg->impl); ToGeneral(s, dg->impl); dg->impl->gpuWritten = true; GlobalBarrier(s.cb);
        VkImageResolve r{}; r.srcSubresource = {sg->impl->aspect, 0, 0, 1}; r.dstSubresource = {dg->impl->aspect, 0, 0, 1};
        r.extent = {sg->impl->width, sg->impl->height, 1};
        F.vkCmdResolveImage(s.cb, sg->impl->image, VK_IMAGE_LAYOUT_GENERAL, dg->impl->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r); GlobalBarrier(s.cb); break;
      }
      case Op::Barrier: EndRendering(s); GlobalBarrier(s.cb); break;
      case Op::FenceSync: { Sync* y = (Sync*)c.p[0]; if (y) { y->value = s.value; y->signaled = false; y->eventId = 1; } break; }
      case Op::UpdateUniform: {
        VkBuffer b; uint64_t o;
        if (!ResolveBuffer((uint64_t)c.l[0] + (uint64_t)c.l[2], (uint64_t)c.l[3], &b, &o)) { LogOnce("upd", "UpdateUniformBuffer address unresolved"); break; }
        EndRendering(s); GlobalBarrier(s.cb);
        uint64_t sz = (c.blob.size() + 3) & ~3ull; std::vector<uint8_t> d(c.blob); d.resize(sz);
        for (uint64_t done = 0; done < sz; done += 65536) { uint64_t n = std::min<uint64_t>(65536, sz - done); F.vkCmdUpdateBuffer(s.cb, b, o + done, n, d.data() + done); }
        GlobalBarrier(s.cb); break;
      }
      case Op::CallCommands:
        if (depth < 8) for (int k = 0; k < c.i[0]; k++) {
          uint64_t h; memcpy(&h, c.blob.data() + 8 * k, 8); auto sub = FindList(h);
          if (!sub) { LogOnce("badcall", "CallCommands with an unknown command handle 0x%llx", (unsigned long long)h); continue; }
          s.keep->push_back(sub); Exec(s, *sub, depth + 1);
        }
        break;
      case Op::ReportCounter: LogOnce("counter", "ReportCounter is a no-op (counter memory stays zero)"); break;
      case Op::TiledCache: case Op::Nop: break;
      default: break;
    }
  }
}

// ---- queue / submit ---------------------------------------------------------------------------------------------------
bool QueueCreate(QueueImpl* q) {
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pi.queueFamilyIndex = g_vk.family;
  return g_vk.f.vkCreateCommandPool(g_vk.dev, &pi, nullptr, &q->pool) == VK_SUCCESS;
}
void QueueDestroy(QueueImpl* q) {
  QueueFinish(q);
  if (q->pool) g_vk.f.vkDestroyCommandPool(g_vk.dev, q->pool, nullptr);
  q->pool = VK_NULL_HANDLE;
}

static void FinishSubmission(VkCommandPool pool, VkCommandBuffer cb, std::shared_ptr<Scratch> sc, std::shared_ptr<DescPools> dp,
                             std::shared_ptr<std::vector<std::shared_ptr<CmdList>>> keep) {
  g_vk.f.vkFreeCommandBuffers(g_vk.dev, pool, 1, &cb);
  { std::lock_guard<std::recursive_mutex> lk(g_vk.lock); for (auto& b : sc->used) g_freeChunks.push_back(b); }
  for (auto d : dp->pools) g_vk.f.vkDestroyDescriptorPool(g_vk.dev, d, nullptr);
  keep->clear();
}

void QueueSubmit(QueueImpl* q, uint32_t n, const uint64_t* handles) {
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  auto& F = g_vk.f;
  EnsureGlobals();
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = q->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
  VkCommandBuffer cb; if (F.vkAllocateCommandBuffers(g_vk.dev, &ai, &cb) != VK_SUCCESS) { Err("vkAllocateCommandBuffers failed"); return; }
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  F.vkBeginCommandBuffer(cb, &bi);
  auto sc = std::make_shared<Scratch>(); auto dp = std::make_shared<DescPools>();
  auto keep = std::make_shared<std::vector<std::shared_ptr<CmdList>>>();
  uint64_t value = NextTimeline();
  for (uint32_t i = 0; i < n; i++) {
    auto l = FindList(handles[i]);
    if (!l) { LogOnce("badh", "SubmitCommands with an unknown command handle 0x%llx", (unsigned long long)handles[i]); continue; }
    keep->push_back(l);
    RS s; s.cb = cb; s.value = value; s.scratch = sc.get(); s.dpools = dp.get(); s.keep = keep.get(); s.q = q; s.dev = q->dev;
    if (q->dev) { s.depthMode = q->dev->depthMode; s.originMode = q->dev->originMode; }
    s.texPool = q->texPool; s.sampPool = q->sampPool;       // Unity binds the pools once and draws from later command buffers
    Exec(s, *l, 0);
    q->texPool = s.texPool; q->sampPool = s.sampPool;
    EndRendering(s);
  }
  F.vkEndCommandBuffer(cb);
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; cbi.commandBuffer = cb;
  VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; sig.semaphore = g_vk.timeline; sig.value = value; sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi; si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &sig;
  VkResult r = F.vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
  if (r != VK_SUCCESS) Err("vkQueueSubmit2 failed (%d)", r);
  q->lastSignal = value; g_stats.submits++;
  VkCommandPool pool = q->pool;
  DeferUntil(value, [=]() { FinishSubmission(pool, cb, sc, dp, keep); });
  RunDeferred();
}

void QueueFinish(QueueImpl* q) {
  if (!g_vk.inited) return;
  WaitTimeline(q->lastSignal, UINT64_MAX);
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  RunDeferred();
}

void QueueFence(QueueImpl* q, Sync* s) {
  // a fence made outside a command buffer signals once everything submitted so far completes
  s->value = q->lastSignal; s->signaled = false;
}

// ---- window ---------------------------------------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

bool WindowCreate(WindowImpl* w) {
  if (!g_vk.hasSwapchain || !g_vk.f.vkCreateWin32SurfaceKHR) return true;     // offscreen: the game's textures are the target
  WNDCLASSA wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "SrNvnHidden";
  RegisterClassA(&wc);
  // A real window handle for the surface. Never shown: no ShowWindow, no WS_VISIBLE, created as a tool window off screen.
  w->hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "SrNvnHidden", "nvn-hidden", WS_POPUP, -32000, -32000, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
  if (!w->hwnd) return true;
  VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; si.hinstance = wc.hInstance; si.hwnd = w->hwnd;
  if (g_vk.f.vkCreateWin32SurfaceKHR(g_vk.inst, &si, nullptr, &w->surface) != VK_SUCCESS) { w->surface = VK_NULL_HANDLE; return true; }
  VkBool32 sup = VK_FALSE; g_vk.f.vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.family, w->surface, &sup);
  if (!sup) { g_vk.f.vkDestroySurfaceKHR(g_vk.inst, w->surface, nullptr); w->surface = VK_NULL_HANDLE; return true; }
  VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  g_vk.f.vkCreateSemaphore(g_vk.dev, &sc, nullptr, &w->acquireSem);
  w->useSwapchain = true;
  return true;
}

void WindowDestroy(WindowImpl* w) {
  if (!g_vk.inited) return;
  g_vk.f.vkDeviceWaitIdle(g_vk.dev);
  if (w->swap) g_vk.f.vkDestroySwapchainKHR(g_vk.dev, w->swap, nullptr);
  if (w->acquireSem) g_vk.f.vkDestroySemaphore(g_vk.dev, w->acquireSem, nullptr);
  for (VkSemaphore s : w->presentSems) g_vk.f.vkDestroySemaphore(g_vk.dev, s, nullptr);
  if (w->surface) g_vk.f.vkDestroySurfaceKHR(g_vk.inst, w->surface, nullptr);
  if (w->hwnd) DestroyWindow(w->hwnd);
  *w = WindowImpl{};
}

static bool EnsureSwapchain(WindowImpl* w, VkFormat want, uint32_t width, uint32_t height) {
  auto& F = g_vk.f;
  if (w->swap && w->swapFormat == want && w->swapExtent.width == width && w->swapExtent.height == height) return true;
  F.vkDeviceWaitIdle(g_vk.dev);
  VkSurfaceCapabilitiesKHR caps; F.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, w->surface, &caps);
  uint32_t nf = 0; F.vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, w->surface, &nf, nullptr);
  std::vector<VkSurfaceFormatKHR> fm(nf); F.vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, w->surface, &nf, fm.data());
  VkSurfaceFormatKHR pick = fm.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fm[0];
  for (auto& f : fm) if (f.format == want) pick = f;
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) ext = {width, height};
  ext.width = std::max(caps.minImageExtent.width, std::min(ext.width, caps.maxImageExtent.width));
  ext.height = std::max(caps.minImageExtent.height, std::min(ext.height, caps.maxImageExtent.height));
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = w->surface; ci.minImageCount = std::max(2u, caps.minImageCount); ci.imageFormat = pick.format; ci.imageColorSpace = pick.colorSpace; ci.imageExtent = ext;
  ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.clipped = VK_TRUE; ci.oldSwapchain = w->swap;
  VkSwapchainKHR ns = VK_NULL_HANDLE;
  if (F.vkCreateSwapchainKHR(g_vk.dev, &ci, nullptr, &ns) != VK_SUCCESS) { LogOnce("swapfail", "swapchain creation failed; presenting offscreen only"); w->useSwapchain = false; return false; }
  if (w->swap) F.vkDestroySwapchainKHR(g_vk.dev, w->swap, nullptr);
  w->swap = ns; w->swapFormat = pick.format; w->swapExtent = ext;
  uint32_t ni = 0; F.vkGetSwapchainImagesKHR(g_vk.dev, ns, &ni, nullptr); w->swapImages.resize(ni); F.vkGetSwapchainImagesKHR(g_vk.dev, ns, &ni, w->swapImages.data());
  for (VkSemaphore s : w->presentSems) F.vkDestroySemaphore(g_vk.dev, s, nullptr);      // the device is idle (waited above)
  w->presentSems.assign(ni, VK_NULL_HANDLE);
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto& s : w->presentSems) F.vkCreateSemaphore(g_vk.dev, &sci, nullptr, &s);
  return true;
}

void QueuePresent(QueueImpl* q, WindowImpl* w, uint32_t idx) {
  w->presents++;
  if (!w->useSwapchain || idx >= w->textures.size() || !w->textures[idx] || !w->textures[idx]->impl) return;
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  auto& F = g_vk.f;
  TextureImpl* t = w->textures[idx]->impl;
  bool srgb = t->vkFormat == VK_FORMAT_R8G8B8A8_SRGB || t->vkFormat == VK_FORMAT_B8G8R8A8_SRGB;
  if (!EnsureSwapchain(w, srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM, t->width, t->height)) return;
  uint32_t img = 0;
  VkResult r = F.vkAcquireNextImageKHR(g_vk.dev, w->swap, UINT64_MAX, w->acquireSem, VK_NULL_HANDLE, &img);
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LogOnce("acq", "vkAcquireNextImageKHR failed (%d)", r); return; }
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = q->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
  VkCommandBuffer cb; F.vkAllocateCommandBuffers(g_vk.dev, &ai, &cb);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; F.vkBeginCommandBuffer(cb, &bi);
  auto trans = [&](VkImage im, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b; F.vkCmdPipelineBarrier2(cb, &di);
  };
  trans(w->swapImages[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  GlobalBarrier(cb);
  VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstSubresource = bl.srcSubresource;
  bl.srcOffsets[1] = {(int32_t)t->width, (int32_t)t->height, 1};
  if (q->dev && q->dev->originMode == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) { bl.srcOffsets[0].y = (int32_t)t->height; bl.srcOffsets[1].y = 0; } bl.dstOffsets[1] = {(int32_t)w->swapExtent.width, (int32_t)w->swapExtent.height, 1};
  F.vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, w->swapImages[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
  trans(w->swapImages[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  F.vkEndCommandBuffer(cb);
  uint64_t value = NextTimeline();
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; cbi.commandBuffer = cb;
  VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; wait.semaphore = w->acquireSem; wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSemaphoreSubmitInfo sig[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
  sig[0].semaphore = g_vk.timeline; sig[0].value = value; sig[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSemaphore presentSem = w->presentSems[img];
  sig[1].semaphore = presentSem; sig[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; si.waitSemaphoreInfoCount = 1; si.pWaitSemaphoreInfos = &wait; si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi;
  si.signalSemaphoreInfoCount = 2; si.pSignalSemaphoreInfos = sig;
  F.vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
  q->lastSignal = value;
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &presentSem; pi.swapchainCount = 1; pi.pSwapchains = &w->swap; pi.pImageIndices = &img;
  F.vkQueuePresentKHR(g_vk.queue, &pi);
  WaitTimeline(value, UINT64_MAX);      // keep the one set of semaphores reusable
  VkCommandPool pool = q->pool; DeferUntil(value, [=]() { g_vk.f.vkFreeCommandBuffers(g_vk.dev, pool, 1, &cb); });
}

// ---- readback (tests, captures) ---------------------------------------------------------------------------------------
bool ReadbackTexture(TextureImpl* t, uint32_t level, uint32_t layer, std::vector<uint8_t>* out, uint32_t* ow, uint32_t* oh) {
  auto& F = g_vk.f;
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  uint32_t w = std::max(1u, t->width >> level), h = std::max(1u, t->height >> level);
  uint32_t bytes = NvnFormatBytes(t->nvnFormat); if (!bytes) bytes = 4;
  Buf st; VkDeviceSize sz = (VkDeviceSize)w * h * bytes;
  if (!CreateBuf(&st, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemKind::HostVisible)) return false;
  VkCommandPool pool; VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = g_vk.family; F.vkCreateCommandPool(g_vk.dev, &pi, nullptr, &pool);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
  VkCommandBuffer cb; F.vkAllocateCommandBuffers(g_vk.dev, &ai, &cb);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; F.vkBeginCommandBuffer(cb, &bi);
  GlobalBarrier(cb);
  VkBufferImageCopy c{}; c.imageSubresource = {t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect, level, layer, 1}; c.imageExtent = {w, h, 1};
  F.vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &c);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2}; mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb; F.vkCmdPipelineBarrier2(cb, &di);
  F.vkEndCommandBuffer(cb);
  uint64_t value = NextTimeline();
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; cbi.commandBuffer = cb;
  VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; sig.semaphore = g_vk.timeline; sig.value = value; sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi; si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &sig;
  F.vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
  bool ok = WaitTimeline(value, 10ull * 1000000000ull);
  if (ok) { out->assign((uint8_t*)st.map, (uint8_t*)st.map + sz); if (ow) *ow = w; if (oh) *oh = h; }
  F.vkDestroyCommandPool(g_vk.dev, pool, nullptr);
  DestroyBuf(&st);
  return ok;
}

}  // namespace nvn
