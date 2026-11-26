// Star Road Recomp: NVN on Vulkan, command replay, pipelines, queue, window (task-12). Our own code (D-008).
//
// Command buffers record NVN calls into a CmdList. Queue submission replays the lists into one Vulkan command buffer with a
// state machine that mirrors NVN's bound state; draws build (and cache) a pipeline from the current state and the shader cache
// manifest (docs/SHADER_CACHE.md). Every image lives in GENERAL layout; hazards use global barriers at render pass boundaries
// and at NVN Barrier calls.
#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>

#include "../gfx/texture/texture.h"
#include "../gfx/upscale/overlay_hook.h"
#include "../gfx/upscale/upscale.h"
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
      if (!CreateBuf(&nb, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, MemKind::HostVisible)) return false;
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
struct LinkedFragment { VkShaderModule module; bool usesOriginal; };
static std::unordered_map<std::string, LinkedFragment> g_linkedFragments;
static VkDescriptorSetLayout g_emptySet = VK_NULL_HANDLE;
static VkBuffer g_nullVb = VK_NULL_HANDLE; static Buf g_nullVbBuf;
struct Dummy { VkImage img = VK_NULL_HANDLE; MemAlloc mem; VkImageView view = VK_NULL_HANDLE; };
static Dummy g_dummy[DIM_COUNT];         // by manifest dim: 1d 2d 3d cube 2d_array cube_array buffer 2dms 2dms_array
// The dummy a binding of this dimension falls back to (a texel buffer has none and reads the cube array's).
static int DummyIdx(int dim) { return dim == DIM_BUFFER ? DIM_CUBE_ARRAY : std::max(0, std::min(dim, (int)DIM_COUNT - 1)); }
static VkSampler g_dummySampler = VK_NULL_HANDLE;
static bool g_dummyInit = false;

void ReplayShutdown() {
  if (!g_vk.inited) return;
  auto& F = g_vk.f;
  F.vkDeviceWaitIdle(g_vk.dev);
  DestroyMaskedClearPipelines();
  for (auto& kv : g_pipes) if (kv.second) F.vkDestroyPipeline(g_vk.dev, kv.second, nullptr);
  g_pipes.clear();
  for (const auto& entry : g_linkedFragments) {
    if (entry.second.module) F.vkDestroyShaderModule(g_vk.dev, entry.second.module, nullptr);
  }
  g_linkedFragments.clear();
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

using ShaderStages = std::array<StageProgram*, 6>;
static constexpr int kGraphicsStageCount = NVN_SHADER_STAGE_COMPUTE;

template <class T> static void Put(std::string& k, const T& v) { k.append((const char*)&v, sizeof v); }

static VkShaderModule LinkFragment(const ShaderStages& stages) {
  StageProgram* fragment = stages[NVN_SHADER_STAGE_FRAGMENT];
  if (!fragment) return VK_NULL_HANDLE;
  StageProgram* producer = stages[NVN_SHADER_STAGE_GEOMETRY];
  if (!producer) producer = stages[NVN_SHADER_STAGE_TESS_EVALUATION];
  if (!producer) producer = stages[NVN_SHADER_STAGE_VERTEX];

  std::string key = producer->key + ":" + fragment->key;
  auto cached = g_linkedFragments.find(key);
  if (cached != g_linkedFragments.end()) {
    return cached->second.usesOriginal ? fragment->module : cached->second.module;
  }

  std::vector<uint32_t> code;
  LinkedFragment linked{VK_NULL_HANDLE, false};
  if (ShaderLinkFragment(*producer, *fragment, code)) {
    if (code.empty()) linked.usesOriginal = true;
    else {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = code.size() * sizeof(uint32_t);
      info.pCode = code.data();
      if (g_vk.f.vkCreateShaderModule(g_vk.dev, &info, nullptr, &linked.module) != VK_SUCCESS) linked.module = VK_NULL_HANDLE;
    }
  }
  if (!linked.module && !linked.usesOriginal) Err("cannot link fragment shader %.12s to producer %.12s", fragment->key.c_str(), producer->key.c_str());
  g_linkedFragments.emplace(std::move(key), linked);
  return linked.usesOriginal ? fragment->module : linked.module;
}

static VkPipelineLayout GetLayout(const ShaderStages& stages) {
  uint32_t nsets = 0;
  VkDescriptorSetLayout sets[6];
  for (auto& s : sets) s = g_emptySet;
  std::string key;
  for (int s = 0; s < 6; s++) {
    StageProgram* sp = stages[s];
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
  ProgramImpl* stageOwners[6] = {};
  uint32_t vertexBindMask = 0;
  const char* missingProgramReason = "no bind recorded on this queue";
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

// Crash breadcrumb: where the replay was when the process faulted (a Vulkan driver fault is otherwise anonymous). Written under the Vulkan lock.
struct Crumb { const char* where = "idle"; int op = -1; size_t index = 0; int depth = 0; bool active = false; uint64_t a = 0, b = 0; };
static Crumb g_crumb;
static LONG CALLBACK CrumbVeh(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_crumb.active)
    fprintf(stderr, "[nvn] access violation during replay (a crash unless handled later): where=%s op=%d cmd#%zu depth=%d a=0x%llx b=0x%llx fault=%p rip=%p\n", g_crumb.where, g_crumb.op, g_crumb.index,
            g_crumb.depth, (unsigned long long)g_crumb.a, (unsigned long long)g_crumb.b,
            ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr, ep->ExceptionRecord->ExceptionAddress), fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

static void ToGeneral(RS& s, TextureImpl* t) {
  g_crumb.where = "ToGeneral"; g_crumb.a = (uint64_t)t; g_crumb.b = t ? (uint64_t)t->image : 0;
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

// The live TextureImpl behind a guest NVNtexture, or null. The game copies NVNtexture structs by value (CRI does, for its movie textures), so a
// struct can keep an impl pointer after another copy was finalized: a recorded command must never follow it blindly.
static TextureImpl* TexOf(const TextureG* g) {
  if (!g) return nullptr;
  TextureImpl* t = g->impl;
  return t && IsLive(t) && t->image ? t : nullptr;
}

// Copy swizzled guest texels into the image (task-11 deswizzle, decode when the GPU cannot take the format).
static void UploadTexture(RS& s, TextureImpl* t, TextureG* g) {
  g_crumb.where = "UploadTexture";
  Pool* pool = t->pool;
  t->uploaded = true;                 // one attempt per guest write; FlushMappedRange clears it
  if (!pool || !pool->memory) return;
  if (IsDepthFormat(t->vkFormat)) return;
  sr_tex_format fmt;
  if (sr_tex_format_from_nvn(g->format, &fmt) != SR_TEX_OK) { LogOnce("upfmt", "texture upload: format 0x%x not handled by task-11", g->format); return; }
  if (((g->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || ((g->flags & NVN_TEXTURE_FLAGS_VIDEO_DECODE) && g->stride)) && !t->is3D && t->layers == 1 && t->levels == 1) {
    // Pitch-linear storage (task-21: the CRIWARE movie texture has VIDEO_DECODE | COMPRESSIBLE flags and a stride of 0x1e00, no LINEAR flag): rows are `stride` bytes apart, no GOB swizzle. One level, one layer.
    const sr_tex_format_info* fi = sr_tex_format_info_get(fmt.fmt);
    if (!fi || fi->block_w != 1 || fi->block_h != 1 || !fi->bytes_per_block) { LogOnce("uplin", "linear texture upload: format 0x%x is not a raw format", g->format); return; }
    uint64_t rowBytes = (uint64_t)t->width * fi->bytes_per_block;
    uint64_t pitch = g->stride ? g->stride : rowBytes;
    uint64_t need = pitch * (t->height - 1) + rowBytes;
    if (pitch < rowBytes || pitch % fi->bytes_per_block || need > (pool->size > t->poolOffset ? pool->size - t->poolOffset : 0)) {
      LogOnce("uplin2", "linear texture upload: stride %llu does not fit %ux%u fmt 0x%x (pool %llu bytes past offset %llu)", (unsigned long long)pitch, t->width, t->height, g->format,
              (unsigned long long)pool->size, (unsigned long long)t->poolOffset);
      return;
    }
    VkBuffer sb; VkDeviceSize so; void* sp;
    if (!s.scratch->Alloc(need, 16, &sb, &so, &sp)) return;
    memcpy(sp, pool->memory + t->poolOffset, (size_t)need);
    VkBufferImageCopy cp{};
    cp.bufferOffset = so; cp.bufferRowLength = (uint32_t)(pitch / fi->bytes_per_block); cp.bufferImageHeight = t->height;
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {t->width, t->height, 1};
    ToGeneral(s, t);
    EndRendering(s);
    GlobalBarrier(s.cb);
    g_vk.f.vkCmdCopyBufferToImage(s.cb, sb, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    GlobalBarrier(s.cb);
    g_stats.uploads++;
    LogOnce("uplin3", "linear texture upload: %ux%u fmt 0x%x stride %llu", t->width, t->height, g->format, (unsigned long long)pitch);
    return;
  }
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
  TextureImpl* t = TexOf(g);
  if (!t) return;
  ToGeneral(s, t);
  if (!t->uploaded && !t->gpuWritten && t->samples <= 1) UploadTexture(s, t, g);     // a multisample image is only ever rendered to
  t->lastUse = s.value;
}

static void InitDummies(RS& s) {
  if (g_dummyInit) return;
  g_dummyInit = true;
  EndRendering(s);                    // the clears and layout changes below are not allowed inside a render pass
  auto& F = g_vk.f;
  struct D { int dim; VkImageType type; VkImageViewType vt; uint32_t layers; VkImageCreateFlags fl; VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT; } ds[DIM_COUNT] = {
    {0, VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, 0}, {1, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0}, {2, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0},
    {3, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT}, {4, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 2, 0},
    {5, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT}, {6, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0},
    {DIM_2DMS, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0, VK_SAMPLE_COUNT_4_BIT}, {DIM_2DMS_ARRAY, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 2, 0, VK_SAMPLE_COUNT_4_BIT}};
  for (auto& d : ds) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = d.fl; ci.imageType = d.type; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {1, 1, 1}; ci.mipLevels = 1; ci.arrayLayers = d.layers;
    ci.samples = d.samples; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (d.samples != VK_SAMPLE_COUNT_1_BIT && !g_vk.feat.features.shaderStorageImageMultisample) ci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;     // multisample storage images are a device feature
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
    if (d.samples != VK_SAMPLE_COUNT_1_BIT) continue;       // a multisample image cannot be cleared by vkCmdClearColorImage; its contents stay undefined
    VkClearColorValue white{{1, 1, 1, 1}}; VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers};
    F.vkCmdClearColorImage(s.cb, m.img, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &rg);
  }
  GlobalBarrier(s.cb);
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.maxLod = 1.0f;
  F.vkCreateSampler(g_vk.dev, &si, nullptr, &g_dummySampler);
}

// ---- pipeline building ---------------------------------------------------------------------------------------------

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
    // no Vulkan equivalent: the draw is turned into an index list of one of these (ExpandPrimitive)
    case NVN_DRAW_PRIMITIVE_LINE_LOOP: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case NVN_DRAW_PRIMITIVE_QUADS: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case NVN_DRAW_PRIMITIVE_POLYGON: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;       // convex polygons: a fan
  }
  return false;
}

static VkFormat RtFormat(const RtBind& r) {
  TextureImpl* t = TexOf(r.tex);
  if (!t) return VK_FORMAT_UNDEFINED;
  if (r.view.flags & 2) { int a, b; VkImageAspectFlags c; VkFormat f = VkFormatFromNvn(r.view.format, &a, &b, &c); if (f != VK_FORMAT_UNDEFINED) return f; }
  return t->vkFormat;
}

static VkPipeline BuildGraphics(RS& s, uint32_t prim, const ShaderStages& shaders) {
  g_crumb.where = "BuildGraphics";
  auto& F = g_vk.f;
  VkPrimitiveTopology topo;
  if (!Topology(prim, &topo)) { LogOnce("badprim", "draw primitive %u has no Vulkan topology (draw skipped)", prim); return VK_NULL_HANDLE; }
  StageProgram* vs = shaders[NVN_SHADER_STAGE_VERTEX];
  VkFormat cfmt[8]; for (uint32_t i = 0; i < s.nColor; i++) cfmt[i] = s.color[i].tex ? RtFormat(s.color[i]) : VK_FORMAT_UNDEFINED;
  VkFormat dfmt = s.depth.tex ? RtFormat(s.depth) : VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  { TextureG* any = s.nColor && s.color[0].tex ? s.color[0].tex : s.depth.tex; if (any) { uint32_t n = std::max(1u, any->samples); samples = (VkSampleCountFlagBits)n; } }

  std::string key;
  Put(key, 'G'); for (auto* shader : shaders) Put(key, shader);
  Put(key, prim); Put(key, s.nColor); for (uint32_t i = 0; i < s.nColor; i++) Put(key, cfmt[i]); Put(key, dfmt); Put(key, samples);
  Put(key, s.colorState); Put(key, s.chan); Put(key, s.poly); Put(key, s.ds); Put(key, s.ms.a2c);
  for (uint32_t i = 0; i < s.nColor; i++) Put(key, s.blend[i]);
  Put(key, s.originMode); Put(key, s.depthClamp); Put(key, s.discard); Put(key, s.primRestart); Put(key, s.sampleMask); Put(key, s.patchSize); Put(key, s.depthMode); Put(key, s.depthBoundsOn);
  Put(key, s.polyFactor != 0 || s.polyUnits != 0);
  for (auto& in : vs->man.inputs) { Put(key, s.attr[in.attr & 15]); Put(key, s.attrSet[in.attr & 15]); Put(key, s.stream[s.attr[in.attr & 15].stream & 15]); }
  auto it = g_pipes.find(key);
  if (it != g_pipes.end()) return it->second;

  VkShaderModule fragment = LinkFragment(shaders);
  if (shaders[NVN_SHADER_STAGE_FRAGMENT] && !fragment) return VK_NULL_HANDLE;
  std::vector<VkPipelineShaderStageCreateInfo> stages;
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg[5];
  for (int st = 0; st < kGraphicsStageCount; st++) {
    StageProgram* sp = shaders[st];
    if (!sp) continue;
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    si.stage = StageBit(st); si.module = st == NVN_SHADER_STAGE_FRAGMENT ? fragment : sp->module; si.pName = "main";
    if (WantSubgroup32(sp, si.stage)) { sg[st] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, nullptr, 32}; si.pNext = &sg[st]; }
    stages.push_back(si);
  }
  // vertex input
  std::vector<VkVertexInputBindingDescription> binds; std::vector<VkVertexInputAttributeDescription> attrs;
  std::vector<VkVertexInputBindingDivisorDescriptionKHR> divisors;
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
    else {
      const auto& stream = s.stream[b];
      d.stride = stream.stride;
      d.inputRate = stream.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
      if (stream.divisor > 1) {
        if (stream.divisor <= g_vk.maxVertexDivisor) divisors.push_back({b, stream.divisor});
        else LogOnce("divisor", "vertex stream divisor %u exceeds device support of %u (treated as 1)", stream.divisor, g_vk.maxVertexDivisor);
      }
    }
    binds.push_back(d);
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineVertexInputDivisorStateCreateInfoKHR vd{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR};
  if (!divisors.empty()) {
    vd.vertexBindingDivisorCount = (uint32_t)divisors.size();
    vd.pVertexBindingDivisors = divisors.data();
    vi.pNext = &vd;
  }
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
  gi.layout = GetLayout(shaders);
  VkPipeline out = VK_NULL_HANDLE;
  VkResult r = F.vkCreateGraphicsPipelines(g_vk.dev, g_pcache, 1, &gi, nullptr, &out);
  if (r != VK_SUCCESS) { Err("vkCreateGraphicsPipelines failed (%d), vertex shader %.12s", r, vs->key.c_str()); out = VK_NULL_HANDLE; g_stats.pipelineFails++; }
  g_pipes[key] = out; g_stats.pipelines++;
  return out;
}

static VkPipeline BuildCompute(const ShaderStages& shaders) {
  g_crumb.where = "BuildCompute";
  StageProgram* cs = shaders[NVN_SHADER_STAGE_COMPUTE];
  std::string key; Put(key, 'C'); Put(key, cs);
  auto it = g_pipes.find(key);
  if (it != g_pipes.end()) return it->second;
  VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
  uint32_t ls[3] = {cs->man.localSize[0], cs->man.localSize[1], cs->man.localSize[2]};
  VkSpecializationInfo sp{3, me, 12, ls};
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, nullptr, 32};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, WantSubgroup32(cs, VK_SHADER_STAGE_COMPUTE_BIT) ? &sg : nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs->module, "main", &sp};
  ci.layout = GetLayout(shaders);
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

static bool BindDescriptors(RS& s, const ShaderStages& shaders, bool compute) {
  g_crumb.where = "BindDescriptors";
  auto& F = g_vk.f;
  InitDummies(s);
  VkPipelineLayout pl = GetLayout(shaders);
  VkPipelineBindPoint bp = compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
  for (int st = 0; st < 6; st++) {
    StageProgram* sp = shaders[st];
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
        VkDescriptorImageInfo di{g_dummySampler, g_dummy[DummyIdx(t.dim)].view, VK_IMAGE_LAYOUT_GENERAL};
        if (t.res == RES_SAMPLER) { tid = UINT32_MAX; sid = tw & 0xFFFFF; }      // a pure sampler word carries only the sampler id
        if (s.texPool && tid < s.texPool->ents.size() && s.texPool->ents[tid].tex) {
          auto& e = s.texPool->ents[tid];
          PrepareSampled(s, e.texG);
          // A shader binding and the texture bound to it must agree on multisampling: an image of the wrong sample count behind a
          // 2dms (or non-2dms) variable is invalid, so the dummy of the binding's own dimension stays in place.
          if ((e.tex->samples > 1) != t.multisample) LogOnce(("msmix" + sp->key + std::to_string(t.binding)).c_str(), "stage %s binding %u is %s but texture id %u has %u samples (dummy used)", StageName(st), t.binding, t.multisample ? "multisample" : "single-sample", tid, e.tex->samples);
          else { VkImageView v = GetView(e.tex, e.texG, e.hasView ? &e.view : nullptr, false); if (v) di.imageView = v; }
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
      VkDescriptorImageInfo di{VK_NULL_HANDLE, g_dummy[DummyIdx(im.dim)].view, VK_IMAGE_LAYOUT_GENERAL};
      uint64_t h = (im.index >= 0 && im.index < 32) ? s.img[st][im.index] : 0;
      uint32_t tid = (uint32_t)h & 0xFFFFF;
      if (h && s.texPool && tid < s.texPool->ents.size() && s.texPool->ents[tid].tex) {
        auto& e = s.texPool->ents[tid]; PrepareSampled(s, e.texG);
        TextureImpl* ti = e.tex; ti->gpuWritten = true;
        bool imMs = im.dim == DIM_2DMS || im.dim == DIM_2DMS_ARRAY;
        if ((ti->samples > 1) == imMs) { VkImageView v = GetView(ti, e.texG, e.hasView ? &e.view : nullptr, false); if (v) di.imageView = v; }
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
  g_crumb.where = "BeginRendering";
  auto& F = g_vk.f;
  if (s.rendering && !s.rtDirty) return true;
  EndRendering(s);
  if (!s.nColor && !s.depth.tex) { LogOnce("nort", "draw with no render targets bound (skipped)"); return false; }
  VkRenderingAttachmentInfo ca[8]; VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX; bool haveDepth = false, haveStencil = false;
  for (uint32_t i = 0; i < s.nColor; i++) if (!s.color[i].tex) { LogOnce("rtgone", "draw with a color target that was finalized or is missing (skipped)"); return false; }
  for (uint32_t i = 0; i < s.nColor; i++) {
    TextureG* g = s.color[i].tex; TextureImpl* t = TexOf(g);
    if (!t) { LogOnce("rtstale", "draw with a color target whose texture is gone (skipped)"); return false; }
    ToGeneral(s, t); t->gpuWritten = true; t->lastUse = s.value;
    VkImageView v = GetView(t, g, &s.color[i].view, true);
    ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca[i].imageView = v; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    uint32_t lv = (s.color[i].view.flags & 8) ? s.color[i].view.minLevel : 0;
    w = std::min(w, std::max(1u, t->width >> lv)); h = std::min(h, std::max(1u, t->height >> lv));
    layers = std::min(layers, (s.color[i].view.flags & 16) ? std::max<uint32_t>(1, s.color[i].view.numLayers) : 1);
  }
  if (s.depth.tex) {
    TextureG* g = s.depth.tex; TextureImpl* t = TexOf(g);
    if (!t) { LogOnce("rtstale", "draw with a depth target whose texture is gone (skipped)"); return false; }
    ToGeneral(s, t); t->gpuWritten = true; t->lastUse = s.value;
    da.imageView = GetView(t, g, &s.depth.view, true); da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    uint32_t lv = (s.depth.view.flags & 8) ? s.depth.view.minLevel : 0;
    w = std::min(w, std::max(1u, t->width >> lv)); h = std::min(h, std::max(1u, t->height >> lv));
    layers = std::min(layers, (s.depth.view.flags & 16) ? std::max<uint32_t>(1, s.depth.view.numLayers) : 1);
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

static VkRect2D RenderScissor(const RS& s) {
  if (!s.scSet) return {{0, 0}, s.area};

  // Intersect the original rectangle with the render area in wide arithmetic.
  const int64_t x = s.scissor[0], y = s.scissor[1];
  const int64_t left = std::clamp<int64_t>(x, 0, s.area.width);
  const int64_t top = std::clamp<int64_t>(y, 0, s.area.height);
  const int64_t right = std::clamp<int64_t>(x + s.scissor[2], left, s.area.width);
  const int64_t bottom = std::clamp<int64_t>(y + s.scissor[3], top, s.area.height);
  return {{(int32_t)left, (int32_t)top}, {(uint32_t)(right - left), (uint32_t)(bottom - top)}};
}

static void SetDynamic(RS& s) {
  g_crumb.where = "SetDynamic";
  auto& F = g_vk.f;
  VkViewport v;
  int32_t x = s.vpSet ? s.viewport[0] : 0, y = s.vpSet ? s.viewport[1] : 0, vw = s.vpSet ? s.viewport[2] : (int32_t)s.area.width, vh = s.vpSet ? s.viewport[3] : (int32_t)s.area.height;
  v.x = (float)x; v.minDepth = s.depthN; v.maxDepth = s.depthF;
  if (s.originMode == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) { v.y = (float)y; v.height = (float)vh; }   // row 0 is the bottom: same numbers as GL
  else { v.y = (float)(y + vh); v.height = -(float)vh; }                                              // row 0 is the top, NDC +y up
  v.width = (float)vw;
  F.vkCmdSetViewport(s.cb, 0, 1, &v);
  const VkRect2D sc = RenderScissor(s);
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
static void PrepareResources(RS& s, const ShaderStages& shaders) {
  g_crumb.where = "PrepareResources";
  InitDummies(s);
  for (int st = 0; st < 6; st++) {
    StageProgram* sp = shaders[st];
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

// Primitives Vulkan lacks (LINE_LOOP, QUADS, QUAD_STRIP) are drawn from an index list built here. `at(i)` gives the i-th source index
// (the running vertex number for a non-indexed draw). Returns the number of indices written to `out`.
template <class At> static uint32_t ExpandPrimitive(uint32_t prim, uint32_t n, At at, std::vector<uint32_t>* out) {
  out->clear();
  if (prim == NVN_DRAW_PRIMITIVE_QUADS) {
    for (uint32_t k = 0; k + 3 < n; k += 4) { uint32_t a = at(k), b = at(k + 1), c = at(k + 2), d = at(k + 3); out->insert(out->end(), {a, b, c, a, c, d}); }
  } else if (prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP) {        // quad i is vertices 2i, 2i+1, 2i+3, 2i+2
    for (uint32_t k = 0; k + 3 < n; k += 2) { uint32_t a = at(k), b = at(k + 1), c = at(k + 2), d = at(k + 3); out->insert(out->end(), {a, b, d, a, d, c}); }
  } else if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP) {
    if (n >= 2) { for (uint32_t k = 0; k < n; k++) out->push_back(at(k)); out->push_back(at(0)); }
  }
  return (uint32_t)out->size();
}
static bool NeedsExpand(uint32_t prim) { return prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP || prim == NVN_DRAW_PRIMITIVE_LINE_LOOP; }

struct IndexFormat { VkIndexType type; uint32_t bytes; };
static IndexFormat DrawIndexFormat(int type) {
  switch (type) {
    case NVN_INDEX_TYPE_UNSIGNED_BYTE: return {VK_INDEX_TYPE_UINT8, 1};
    case NVN_INDEX_TYPE_UNSIGNED_SHORT: return {VK_INDEX_TYPE_UINT16, 2};
    case NVN_INDEX_TYPE_UNSIGNED_INT: return {VK_INDEX_TYPE_UINT32, 4};
    default: return {VK_INDEX_TYPE_MAX_ENUM, 0};
  }
}

static bool ResolveIndexBuffer(uint64_t address, IndexFormat format, uint32_t count,
                               VkBuffer* buffer, uint64_t* offset) {
  if (!format.bytes || address % format.bytes != 0 ||
      !ResolveBuffer(address, uint64_t(std::max(count, 1u)) * format.bytes, buffer, offset)) {
    LogOnce("badidx", "draw has an invalid or unresolved index range");
    return false;
  }
  if (format.type == VK_INDEX_TYPE_UINT8 && !g_vk.hasIdx8) {
    LogOnce("idx8", "8-bit indices require the Vulkan indexTypeUint8 feature");
    return false;
  }
  return true;
}

static bool ResolveIndirectBuffer(uint64_t address, size_t size, VkBuffer* buffer, uint64_t* offset) {
  if (address % sizeof(uint32_t) == 0 && ResolveBuffer(address, size, buffer, offset)) return true;
  LogOnce("badindirect", "indirect command arguments are misaligned or outside the memory pool");
  return false;
}

// Upload an expanded index list to scratch memory and bind it. False when the scratch allocation failed.
static bool BindExpanded(RS& s, const std::vector<uint32_t>& idx) {
  VkBuffer b; VkDeviceSize o; void* p;
  if (!s.scratch->Alloc(idx.size() * 4, 4, &b, &o, &p)) return false;
  memcpy(p, idx.data(), idx.size() * 4);
  g_vk.f.vkCmdBindIndexBuffer(s.cb, b, o, VK_INDEX_TYPE_UINT32);
  return true;
}

static bool PrepareDraw(RS& s, uint32_t prim, bool* skip) {
  g_crumb.where = "PrepareDraw";
  *skip = true;
  if (!s.stageOwners[NVN_SHADER_STAGE_VERTEX]) {
    g_stats.noProgNull++;
    const std::string key = std::string("noprog") + s.missingProgramReason + std::to_string(s.vertexBindMask);
    LogOnce(key.c_str(), "draw with no vertex program bound (skipped): %s, last vertex bind mask 0x%x", s.missingProgramReason, s.vertexBindMask);
    return false;
  }
  ShaderStages shaders{};
  for (int st = 0; st < kGraphicsStageCount; st++) {
    ProgramImpl* owner = s.stageOwners[st];
    if (!owner) continue;
    StageProgram* shader = owner->byStage[st];
    if (!owner->complete || (st == NVN_SHADER_STAGE_VERTEX && !shader)) {
      g_stats.noProgIncomplete++;
      std::string why;
      for (StageProgram* sp : owner->stages) {
        if (sp && !sp->ok) why += " " + sp->key.substr(0, 12) + "(" + sp->reason + ")";
      }
      LogOnce(("noprog" + std::to_string(owner->id)).c_str(), "draw with an incomplete program %llu (skipped):%s",
              (unsigned long long)owner->id, why.empty() ? " required stage data unavailable" : why.c_str());
      return false;
    }
    if (shader && !EnsureStage(shader)) return false;
    shaders[st] = shader;
  }
  EnsureGlobals();
  PrepareResources(s, shaders);
  if (!BeginRendering(s)) return false;
  VkPipeline pipe = BuildGraphics(s, prim, shaders);
  if (!pipe) return false;
  g_vk.f.vkCmdBindPipeline(s.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  SetDynamic(s);
  if (!BindDescriptors(s, shaders, false)) return false;
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

static bool DrawMaskedClear(RS& s, const RtBind& target, VkRect2D rect, uint32_t mask, ClearKind kind, const Cmd& c) {
  auto& vk = g_vk.f;
  TextureImpl* texture = TexOf(target.tex);
  if (!texture) return false;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = GetMaskedClearPipeline(RtFormat(target), (VkSampleCountFlagBits)texture->samples,
                                                mask, kind, layout);
  if (!pipeline) {
    LogOnce("clrmask", "cannot create a masked clear pipeline");
    return false;
  }
  // Draw into one attachment and one layer at a time so no guest MRT or shader state is changed.
  TextureView view = target.view;
  const uint32_t base = (view.flags & 16) ? view.minLayer : 0;
  view.flags |= 16;
  view.numLayers = 1;
  std::vector<VkImageView> views;
  views.reserve(s.renderLayers);
  for (uint32_t i = 0; i < s.renderLayers; i++) {
    view.minLayer = base + i;
    VkImageView attachment = GetView(texture, target.tex, &view, true);
    if (!attachment) return false;
    views.push_back(attachment);
  }
  EndRendering(s);
  VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
  render.renderArea = rect;
  render.layerCount = 1;
  const bool stencil = kind == ClearKind::Stencil;
  if (stencil) render.pStencilAttachment = &attachment;
  else {
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &attachment;
  }
  const VkViewport viewport{(float)rect.offset.x, (float)rect.offset.y, (float)rect.extent.width,
                           (float)rect.extent.height, 0.0f, 1.0f};
  for (VkImageView layer : views) {
    attachment.imageView = layer;
    vk.vkCmdBeginRendering(s.cb, &render);
    vk.vkCmdBindPipeline(s.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vk.vkCmdSetViewport(s.cb, 0, 1, &viewport);
    vk.vkCmdSetScissor(s.cb, 0, 1, &rect);
    if (stencil) {
      vk.vkCmdSetStencilReference(s.cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)c.i[1] & 0xff);
      vk.vkCmdSetStencilWriteMask(s.cb, VK_STENCIL_FACE_FRONT_AND_BACK, mask);
    } else vk.vkCmdPushConstants(s.cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c.st);
    vk.vkCmdDraw(s.cb, 3, 1, 0, 0);
    vk.vkCmdEndRendering(s.cb);
  }
  GlobalBarrier(s.cb);
  return true;
}

static void DoClearColor(RS& s, const Cmd& c) {
  g_crumb.where = "DoClearColor";
  const uint32_t mask = (uint32_t)c.i[1] & NVN_CLEAR_COLOR_MASK_RGBA;
  if (!mask) return;
  uint32_t idx = (uint32_t)c.i[0];
  if (idx >= s.nColor || !s.color[idx].tex) { LogOnce("clr", "ClearColor of target %u that is not bound", idx); return; }
  if (!BeginRendering(s)) return;
  const VkClearRect cr{RenderScissor(s), 0, s.renderLayers};
  if (!cr.rect.extent.width || !cr.rect.extent.height) return;
  if (mask == NVN_CLEAR_COLOR_MASK_RGBA) {
    VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = idx; memcpy(&ca.clearValue.color, c.st, 16);
    g_vk.f.vkCmdClearAttachments(s.cb, 1, &ca, 1, &cr);
  } else if (!DrawMaskedClear(s, s.color[idx], cr.rect, mask, (ClearKind)c.i[2], c)) return;
  g_stats.clears++;
}

static void DoClearDepth(RS& s, const Cmd& c) {
  g_crumb.where = "DoClearDepth";
  const TextureImpl* texture = TexOf(s.depth.tex);
  if (!texture) { LogOnce("clrd", "ClearDepthStencil with no depth-stencil target bound"); return; }
  const uint32_t stencilMask = (texture->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? (uint32_t)c.i[2] & 0xff : 0;
  VkImageAspectFlags aspects = (c.i[0] && (texture->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
  if (stencilMask == 0xff) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
  if (!aspects && !stencilMask) return;
  if (!BeginRendering(s)) return;
  const VkClearRect cr{RenderScissor(s), 0, s.renderLayers};
  if (!cr.rect.extent.width || !cr.rect.extent.height) return;
  if (aspects) {
    VkClearAttachment ca{}; ca.aspectMask = aspects; ca.clearValue.depthStencil = {c.f[0], (uint32_t)c.i[1] & 0xff};
    g_vk.f.vkCmdClearAttachments(s.cb, 1, &ca, 1, &cr);
  }
  const bool partialStencil = stencilMask && stencilMask != 0xff;
  if (partialStencil && DrawMaskedClear(s, s.depth, cr.rect, stencilMask, ClearKind::Stencil, c)) {
    g_stats.clears++;
  } else if (aspects) g_stats.clears++;
}

struct ImageCopyRegion {
  VkImageSubresourceLayers layers;
  VkOffset3D offset;
  VkExtent3D extent;

  VkOffset3D End() const {
    return {offset.x + (int32_t)extent.width, offset.y + (int32_t)extent.height, offset.z + (int32_t)extent.depth};
  }

  ImageCopyRegion Slice(bool volume, uint32_t index) const {
    ImageCopyRegion slice = *this;
    if (volume) {
      slice.offset.z += (int32_t)index;
      slice.extent.depth = 1;
    } else {
      slice.layers.baseArrayLayer += index;
      slice.layers.layerCount = 1;
    }
    return slice;
  }
};

static bool ResolveImageRegion(const TextureImpl& texture, const TextureView& view, const int32_t* region,
                               ImageCopyRegion& copy) {
  const uint32_t mip = (view.flags & 8) ? view.minLevel : 0;
  if (mip >= texture.levels || mip >= 32 || region[0] < 0 || region[1] < 0 ||
      region[2] < 0 || region[3] <= 0 || region[4] <= 0 || region[5] < 0) return false;
  const uint32_t width = std::max(1u, texture.width >> mip);
  const uint32_t height = std::max(1u, texture.height >> mip);
  const uint32_t count = std::max(1, region[5]);
  if (uint64_t(region[0]) + region[3] > width || uint64_t(region[1]) + region[4] > height) return false;

  const VkImageAspectFlags aspect = (texture.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : texture.aspect;
  copy.layers = {aspect, mip, 0, 1};
  copy.offset = {region[0], region[1], 0};
  copy.extent = {(uint32_t)region[3], (uint32_t)region[4], 1};
  if (texture.is3D) {
    if (uint64_t(region[2]) + count > std::max(1u, texture.depth >> mip)) return false;
    copy.offset.z = region[2];
    copy.extent.depth = count;
  } else {
    const uint64_t layer = uint64_t((view.flags & 16) ? view.minLayer : 0) + region[2];
    if (layer + count > texture.layers) return false;
    copy.layers.baseArrayLayer = (uint32_t)layer;
    copy.layers.layerCount = count;
  }
  return true;
}

static bool ResolveViewRegion(const TextureImpl& texture, TextureView& view, VkExtent2D& extent, VkFormat& format) {
  if (texture.is3D || texture.aspect != VK_IMAGE_ASPECT_COLOR_BIT || !texture.renderTarget) return false;
  const uint32_t mip = (view.flags & 8) ? view.minLevel : 0;
  const uint32_t base = (view.flags & 16) ? view.minLayer : 0;
  const uint32_t count = (view.flags & 16) ? view.numLayers : texture.layers;
  if (mip >= texture.levels || mip >= 32 || base >= texture.layers ||
      !count || count > texture.layers - base || count > UINT16_MAX) return false;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  format = (view.flags & 2) ? VkFormatFromNvn(view.format, nullptr, nullptr, &aspect) : texture.vkFormat;
  if (format == VK_FORMAT_UNDEFINED || aspect != VK_IMAGE_ASPECT_COLOR_BIT) return false;
  extent = {std::max(1u, texture.width >> mip), std::max(1u, texture.height >> mip)};
  view.flags |= 8 | 16;
  view.minLevel = (uint16_t)mip; view.numLevels = 1;
  view.minLayer = (uint16_t)base; view.numLayers = (uint16_t)count;
  return true;
}

static void DoDownsample(RS& s, const Cmd& c) {
  auto* src = (TextureG*)c.obj[0];
  auto* dst = (TextureG*)c.obj[1];
  TextureImpl* source = TexOf(src);
  TextureImpl* dest = TexOf(dst);
  if (!source || !dest) return;
  TextureView sv{}, dv{};
  if (!c.blob.empty()) {
    sv = ViewAt(c.blob.data());
    dv = ViewAt(c.blob.data() + sizeof(TextureView));
  }
  VkExtent2D extent{}, destExtent{};
  VkFormat format, destFormat;
  if (source->samples <= 1 || dest->samples != 1 ||
      !ResolveViewRegion(*source, sv, extent, format) || !ResolveViewRegion(*dest, dv, destExtent, destFormat) ||
      extent.width != destExtent.width || extent.height != destExtent.height ||
      sv.numLayers != dv.numLayers || format != destFormat) {
    LogOnce("badresolve", "Downsample needs matching color views from multisample to single-sample textures");
    return;
  }
  const VkImageView sourceView = GetView(source, src, &sv, true);
  const VkImageView destView = GetView(dest, dst, &dv, true);
  if (!sourceView || !destView) return;

  EndRendering(s);
  ToGeneral(s, source);
  ToGeneral(s, dest);
  GlobalBarrier(s.cb);
  source->lastUse = s.value;
  dest->gpuWritten = true;
  dest->uploaded = true;
  dest->lastUse = s.value;

  // Attachment resolves apply the selected view format, including integer sample-zero behavior.
  VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = sourceView;
  attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  attachment.resolveMode = IsIntegerFormat(format) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
  attachment.resolveImageView = destView;
  attachment.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
  render.renderArea = {{0, 0}, extent};
  render.layerCount = sv.numLayers;
  render.colorAttachmentCount = 1;
  render.pColorAttachments = &attachment;
  g_vk.f.vkCmdBeginRendering(s.cb, &render);
  g_vk.f.vkCmdEndRendering(s.cb);
  GlobalBarrier(s.cb);
}

struct BufferImageLayout {
  VkBuffer buffer = VK_NULL_HANDLE;
  uint64_t offset = 0;
  uint64_t separateSlicePitch = 0;
  uint32_t rowLength = 0;
};

static bool ResolveImageBuffer(const TextureImpl& texture, const ImageCopyRegion& region, const Cmd& copy,
                               BufferImageLayout& layout) {
  if (texture.samples != 1 || copy.l[1] < 0 || copy.l[2] < 0) return false;
  uint32_t blockWidth = 1, blockHeight = 1, blockBytes = NvnFormatBytes(texture.nvnFormat);
  const bool depthStencil = region.layers.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT;
  if (region.layers.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) {
    blockBytes = texture.vkFormat == VK_FORMAT_D16_UNORM ? 2 : 4;
  } else if (region.layers.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
    blockBytes = 1;
  } else {
    sr_tex_format format{};
    if (sr_tex_format_from_nvn(texture.nvnFormat, &format) == SR_TEX_OK) {
      const sr_tex_format_info* info = sr_tex_format_info_get(format.fmt);
      if (info && info->codec != SR_TEX_CODEC_RAW) {
        // An emulated ASTC image needs a conversion from guest blocks, not a direct buffer copy.
        if (info->codec == SR_TEX_CODEC_ASTC &&
            (texture.vkFormat == VK_FORMAT_R8G8B8A8_UNORM || texture.vkFormat == VK_FORMAT_R8G8B8A8_SRGB)) return false;
        blockWidth = info->block_w; blockHeight = info->block_h; blockBytes = info->bytes_per_block;
      }
    }
  }
  if (!blockBytes || !blockWidth || !blockHeight) return false;
  const uint32_t mip = region.layers.mipLevel;
  const auto end = region.End();
  if (region.offset.x % blockWidth || region.offset.y % blockHeight ||
      (region.extent.width % blockWidth && (uint32_t)end.x != std::max(1u, texture.width >> mip)) ||
      (region.extent.height % blockHeight && (uint32_t)end.y != std::max(1u, texture.height >> mip))) return false;

  const uint64_t rowBytes = ((uint64_t(region.extent.width) + blockWidth - 1) / blockWidth) * blockBytes;
  const uint64_t rowPitch = copy.l[1] ? (uint64_t)copy.l[1] : rowBytes;
  if (rowPitch < rowBytes || rowPitch > INT32_MAX || rowPitch % blockBytes) return false;
  if (rowPitch != rowBytes) {
    const uint64_t texels = rowPitch / blockBytes * blockWidth;
    if (texels > UINT32_MAX) return false;
    layout.rowLength = (uint32_t)texels;
  }
  const uint64_t rows = (uint64_t(region.extent.height) + blockHeight - 1) / blockHeight;
  const uint64_t defaultSlicePitch = rows * rowPitch;
  const uint64_t sliceBytes = defaultSlicePitch - rowPitch + rowBytes;
  const uint64_t slices = texture.is3D ? region.extent.depth : region.layers.layerCount;
  const uint64_t slicePitch = copy.l[2] ? (uint64_t)copy.l[2] : defaultSlicePitch;
  const uint32_t alignment = depthStencil ? 4u : blockBytes;
  if (slices > 1) {
    if (slicePitch < sliceBytes || slices - 1 > (UINT64_MAX - sliceBytes) / slicePitch) return false;
    if (slicePitch != defaultSlicePitch) {
      if (slicePitch % alignment) return false;
      layout.separateSlicePitch = slicePitch;
    }
  }
  const uint64_t length = (slices - 1) * slicePitch + sliceBytes;
  return ResolveBuffer((uint64_t)copy.l[0], length, &layout.buffer, &layout.offset) &&
         layout.offset % alignment == 0;
}

static void DoCopy(RS& s, const Cmd& c) {
  g_crumb.where = "DoCopy";
  auto& F = g_vk.f;
  EndRendering(s); GlobalBarrier(s.cb);
  switch (c.op) {
    case Op::CopyBufToBuf: {
      VkBuffer a, b; uint64_t oa, ob;
      if (!ResolveBuffer((uint64_t)c.l[0], (uint64_t)c.l[2], &a, &oa) || !ResolveBuffer((uint64_t)c.l[1], (uint64_t)c.l[2], &b, &ob)) { LogOnce("cpb", "CopyBufferToBuffer with unresolved address"); break; }
      VkBufferCopy r{oa, ob, (VkDeviceSize)c.l[2]}; F.vkCmdCopyBuffer(s.cb, a, b, 1, &r); break;
    }
    case Op::CopyBufToTex: case Op::CopyTexToBuf: {
      const bool upload = c.op == Op::CopyBufToTex;
      TextureG* g = (TextureG*)c.obj[0]; TextureImpl* t = TexOf(g); TextureView v = ViewAt(c.blob.data()); const int32_t* rg = (const int32_t*)(c.blob.data() + 16);
      if (!t) { LogOnce("cpimg", "buffer-image copy with unresolved texture"); break; }
      ImageCopyRegion image{};
      if (!ResolveImageRegion(*t, v, rg, image)) { LogOnce("cpimgregion", "buffer-image copy exceeds the image subresource"); break; }
      BufferImageLayout layout;
      if (!ResolveImageBuffer(*t, image, c, layout)) { LogOnce("cpimgbuffer", "buffer-image copy has an invalid range, stride or format"); break; }
      ToGeneral(s, t);
      if (upload) { t->gpuWritten = true; t->uploaded = true; t->lastUse = s.value; }
      else PrepareSampled(s, g);
      const VkBufferImageCopy packed{layout.offset, layout.rowLength, 0, image.layers, image.offset, image.extent};
      std::vector<VkBufferImageCopy> split;
      const VkBufferImageCopy* regions = &packed;
      uint32_t count = 1;
      if (layout.separateSlicePitch) {
        count = t->is3D ? image.extent.depth : image.layers.layerCount;
        split.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
          const ImageCopyRegion slice = image.Slice(t->is3D, i);
          split.push_back({layout.offset + i * layout.separateSlicePitch, layout.rowLength, 0,
                           slice.layers, slice.offset, slice.extent});
        }
        regions = split.data();
      }
      if (upload) F.vkCmdCopyBufferToImage(s.cb, layout.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, count, regions);
      else F.vkCmdCopyImageToBuffer(s.cb, t->image, VK_IMAGE_LAYOUT_GENERAL, layout.buffer, count, regions);
      break;
    }
    case Op::CopyTexToTex: {
      TextureG* sg = (TextureG*)c.obj[0]; TextureG* dg = (TextureG*)c.obj[1]; TextureImpl* st = TexOf(sg); TextureImpl* dt = TexOf(dg);
      TextureView sv = ViewAt(c.blob.data()); const int32_t* sr = (const int32_t*)(c.blob.data() + 16);
      TextureView dv = ViewAt(c.blob.data() + 40); const int32_t* dr = (const int32_t*)(c.blob.data() + 56);
      if (!st || !dt) break;
      ImageCopyRegion src{}, dst{};
      if (!ResolveImageRegion(*st, sv, sr, src) || !ResolveImageRegion(*dt, dv, dr, dst)) {
        LogOnce("cpttregion", "CopyTextureToTexture region exceeds the source or destination subresource");
        break;
      }
      const bool mixedDimensions = st->is3D != dt->is3D;
      const uint32_t srcSlices = st->is3D ? src.extent.depth : src.layers.layerCount;
      const uint32_t dstSlices = dt->is3D ? dst.extent.depth : dst.layers.layerCount;
      const bool flipX = c.i[0] & NVN_COPY_FLAGS_MIRROR_X_BIT;
      const bool flipY = c.i[0] & NVN_COPY_FLAGS_MIRROR_Y_BIT;
      const bool flipZ = c.i[0] & NVN_COPY_FLAGS_MIRROR_Z_BIT;
      const bool linear = c.i[0] & NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
      const bool sameSize = src.extent.width == dst.extent.width && src.extent.height == dst.extent.height && srcSlices == dstSlices;
      const auto blockCompressed = [](VkFormat format) {
        return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
      };
      // Equal extents still require conversion when the color encodings differ, e.g. packed HDR to sRGB.
      const bool convertColor = st->vkFormat != dt->vkFormat && src.layers.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
          !IsIntegerFormat(st->vkFormat) && !IsIntegerFormat(dt->vkFormat) &&
          !blockCompressed(st->vkFormat) && !blockCompressed(dt->vkFormat);
      const bool copy = sameSize && st->samples == dt->samples && !flipX && !flipY && !convertColor;
      if ((!copy && (mixedDimensions || st->samples != 1 || dt->samples != 1 ||
                     src.layers.layerCount != dst.layers.layerCount)) ||
          src.layers.aspectMask != dst.layers.aspectMask) {
        LogOnce("cpttshape", "CopyTextureToTexture has incompatible aspects, slices or sample counts");
        break;
      }
      if (!copy && (!(st->optimalFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
                    !(dt->optimalFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
                    (linear && !(st->optimalFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))) {
        LogOnce("cpttblitformat", "CopyTextureToTexture blit format or filter is unsupported by the device");
        break;
      }
      PrepareSampled(s, sg); ToGeneral(s, dt); dt->gpuWritten = true; dt->uploaded = true; dt->lastUse = s.value;
      if (copy) {
        if (flipZ && srcSlices > 1) {
          std::vector<VkImageCopy> regions;
          regions.reserve(srcSlices);
          for (uint32_t layer = 0; layer < srcSlices; layer++) {
            const ImageCopyRegion from = src.Slice(st->is3D, srcSlices - 1 - layer);
            const ImageCopyRegion to = dst.Slice(dt->is3D, layer);
            regions.push_back({from.layers, from.offset, to.layers, to.offset, from.extent});
          }
          F.vkCmdCopyImage(s.cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL,
                           (uint32_t)regions.size(), regions.data());
        } else {
          VkImageCopy r{src.layers, src.offset, dst.layers, dst.offset, src.extent};
          if (mixedDimensions) r.extent.depth = srcSlices;
          F.vkCmdCopyImage(s.cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
      } else {
        auto Region = [&](const ImageCopyRegion& from, const ImageCopyRegion& to) {
          VkImageBlit r{from.layers, {from.offset, from.End()}, to.layers, {to.offset, to.End()}};
          if (flipX) std::swap(r.srcOffsets[0].x, r.srcOffsets[1].x);
          if (flipY) std::swap(r.srcOffsets[0].y, r.srcOffsets[1].y);
          if (flipZ && st->is3D) std::swap(r.srcOffsets[0].z, r.srcOffsets[1].z);
          return r;
        };
        const VkFilter filter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (flipZ && !st->is3D && srcSlices > 1) {
          std::vector<VkImageBlit> regions;
          regions.reserve(srcSlices);
          for (uint32_t layer = 0; layer < srcSlices; layer++) {
            regions.push_back(Region(src.Slice(false, srcSlices - 1 - layer), dst.Slice(false, layer)));
          }
          F.vkCmdBlitImage(s.cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL,
                           (uint32_t)regions.size(), regions.data(), filter);
        } else {
          const VkImageBlit r = Region(src, dst);
          F.vkCmdBlitImage(s.cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, filter);
        }
      }
      break;
    }
    default: break;
  }
  GlobalBarrier(s.cb); g_stats.copies++;
}

static void Exec(RS& s, const CmdList& l, int depth) {
  auto& F = g_vk.f;
  size_t cmdIndex = 0;
  for (const Cmd& c : l.cmds) {
    g_crumb.op = (int)c.op; g_crumb.index = cmdIndex++; g_crumb.depth = depth; g_crumb.where = "exec";
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
      case Op::BindProgram: {
        auto* program = IsLive((void*)c.p[0]) ? (ProgramImpl*)c.p[0] : nullptr;
        const uint32_t mask = (uint32_t)c.i[0];
        for (int st = 0; st < 6; st++) if (mask & (1u << st)) s.stageOwners[st] = program;
        if (mask & NVN_SHADER_STAGE_VERTEX_BIT) {
          s.vertexBindMask = mask;
          if (!program) s.missingProgramReason = c.i[1] ? "bound program is unavailable" : "explicit null bind";
        }
        break;
      }
      case Op::BindVertexBuffer: if (c.i[0] >= 0 && c.i[0] < 16) s.vb[c.i[0]] = {(uint64_t)c.l[0], (uint64_t)c.l[1]}; break;
      case Op::BindVertexBuffers: for (int k = 0; k < c.i[1]; k++) if (c.i[0] + k < 16) memcpy(&s.vb[c.i[0] + k], c.blob.data() + 16 * k, 16); break;
      case Op::BindUniform: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 16) s.ub[c.i[0]][c.i[1]] = {(uint64_t)c.l[0], (uint64_t)c.l[1]}; break;
      case Op::BindUniforms: for (int k = 0; k < c.i[2]; k++) if (c.i[0] < 6 && c.i[1] + k < 16) memcpy(&s.ub[c.i[0]][c.i[1] + k], c.blob.data() + 16 * k, 16); break;
      case Op::BindStorage: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 16) s.sb[c.i[0]][c.i[1]] = {(uint64_t)c.l[0], (uint64_t)c.l[1]}; break;
      case Op::BindSepTexture: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 128) s.c2[c.i[0]][(c.i[2] + 8 * c.i[1]) / 4] = (uint32_t)c.l[0]; break;
      case Op::BindSepSampler: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 128) s.c2[c.i[0]][(0x568 + 8 * c.i[1]) / 4] = (uint32_t)c.l[0]; break;
      case Op::BindImage: if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 32) s.img[c.i[0]][c.i[1]] = (uint64_t)c.l[0]; break;
      case Op::SetRenderTargets: {
        uint32_t n = std::min(8, c.i[0]); s.nColor = n;
        const uint8_t* b = c.blob.data();
        for (uint32_t i = 0; i < 8; i++) {
          TextureG* t = nullptr; memcpy(&t, b + 8 * i, 8);
          if (t && !(t->magic == kMagicTexture && TexOf(t))) t = nullptr;        // finalized since it was recorded
          s.color[i].tex = i < n ? t : nullptr;
          s.color[i].view = ViewAt(b + 64 + 16 * i);
        }
        memcpy(&s.depth.tex, b + 64 + 128, 8); s.depth.view = ViewAt(b + 64 + 128 + 8);
        if (s.depth.tex && !(s.depth.tex->magic == kMagicTexture && TexOf(s.depth.tex))) s.depth.tex = nullptr;
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
      case Op::SetTexturePool: s.texPool = IsLive((void*)c.p[0]) ? (TexPool*)c.p[0] : nullptr; break;      // a pool finalized since the command was recorded is gone
      case Op::SetSamplerPool: s.sampPool = IsLive((void*)c.p[0]) ? (SampPool*)c.p[0] : nullptr; break;
      case Op::Draw: case Op::DrawInstanced: {
        bool skip; if (!PrepareDraw(s, (uint32_t)c.i[0], &skip)) { g_stats.skippedDraws++; break; }
        if (NeedsExpand((uint32_t)c.i[0])) {
          std::vector<uint32_t> idx; uint32_t first = (uint32_t)c.i[1];
          if (!ExpandPrimitive((uint32_t)c.i[0], (uint32_t)c.i[2], [&](uint32_t k) { return first + k; }, &idx) || !BindExpanded(s, idx)) { g_stats.skippedDraws++; break; }
          F.vkCmdDrawIndexed(s.cb, (uint32_t)idx.size(), c.op == Op::Draw ? 1u : (uint32_t)c.i[4], 0, 0, c.op == Op::Draw ? 0u : (uint32_t)c.i[3]); g_stats.draws++; break;
        }
        F.vkCmdDraw(s.cb, (uint32_t)c.i[2], c.op == Op::Draw ? 1u : (uint32_t)c.i[4], (uint32_t)c.i[1], c.op == Op::Draw ? 0u : (uint32_t)c.i[3]); g_stats.draws++; break;
      }
      case Op::DrawElements: case Op::DrawElementsInstanced: {
        const IndexFormat format = DrawIndexFormat(c.i[1]);
        if (!format.bytes || c.i[2] < 0) { g_stats.skippedDraws++; break; }
        bool skip; if (!PrepareDraw(s, (uint32_t)c.i[0], &skip)) { g_stats.skippedDraws++; break; }
        VkBuffer ib; uint64_t io;
        if (NeedsExpand((uint32_t)c.i[0])) {
          const uint32_t isz = format.bytes, n = (uint32_t)c.i[2];
          const uint8_t* src = HostPtr((uint64_t)c.l[0], (uint64_t)n * isz);
          if (!src) { LogOnce("badidx2", "DrawElements of QUADS/QUAD_STRIP/LINE_LOOP needs its index data in a memory pool (skipped)"); g_stats.skippedDraws++; break; }
          auto at = [&](uint32_t k) -> uint32_t {
            uint32_t index = 0;
            memcpy(&index, src + uint64_t(k) * isz, isz);
            return index;
          };
          std::vector<uint32_t> idx;
          if (!ExpandPrimitive((uint32_t)c.i[0], n, at, &idx) || !BindExpanded(s, idx)) { g_stats.skippedDraws++; break; }
          F.vkCmdDrawIndexed(s.cb, (uint32_t)idx.size(), c.op == Op::DrawElements ? 1u : (uint32_t)c.i[5], 0, c.i[3], c.op == Op::DrawElements ? 0u : (uint32_t)c.i[4]); g_stats.draws++; break;
        }
        if (!ResolveIndexBuffer((uint64_t)c.l[0], format, (uint32_t)c.i[2], &ib, &io)) { g_stats.skippedDraws++; break; }
        F.vkCmdBindIndexBuffer(s.cb, ib, io, format.type);
        F.vkCmdDrawIndexed(s.cb, (uint32_t)c.i[2], c.op == Op::DrawElements ? 1u : (uint32_t)c.i[5], 0, c.i[3], c.op == Op::DrawElements ? 0u : (uint32_t)c.i[4]); g_stats.draws++; break;
      }
      case Op::DrawIndirect: case Op::DrawElementsIndirect: {
        const uint32_t prim = (uint32_t)c.i[0];
        if (NeedsExpand(prim)) {
          LogOnce("indirecttopology", "indirect draw needs GPU primitive expansion (skipped)");
          g_stats.skippedDraws++; break;
        }
        const bool indexed = c.op == Op::DrawElementsIndirect;
        const size_t argSize = indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
        VkBuffer ib = VK_NULL_HANDLE, db; uint64_t io = 0, dof;
        const IndexFormat format = DrawIndexFormat(c.i[1]);
        if (!ResolveIndirectBuffer((uint64_t)c.l[1], argSize, &db, &dof) ||
            (indexed && !ResolveIndexBuffer((uint64_t)c.l[0], format, 1, &ib, &io))) {
          g_stats.skippedDraws++; break;
        }
        bool skip; if (!PrepareDraw(s, prim, &skip)) { g_stats.skippedDraws++; break; }
        if (indexed) {
          F.vkCmdBindIndexBuffer(s.cb, ib, io, format.type);
          F.vkCmdDrawIndexedIndirect(s.cb, db, dof, 1, (uint32_t)argSize);
        } else F.vkCmdDrawIndirect(s.cb, db, dof, 1, (uint32_t)argSize);
        g_stats.draws++; break;
      }
      case Op::Dispatch: case Op::DispatchIndirect: {
        EndRendering(s);
        VkBuffer indirect = VK_NULL_HANDLE;
        uint64_t offset = 0;
        if (c.op == Op::DispatchIndirect &&
            !ResolveIndirectBuffer((uint64_t)c.l[0], sizeof(VkDispatchIndirectCommand), &indirect, &offset)) break;
        ProgramImpl* owner = s.stageOwners[NVN_SHADER_STAGE_COMPUTE];
        StageProgram* shader = owner && owner->complete ? owner->byStage[NVN_SHADER_STAGE_COMPUTE] : nullptr;
        if (!shader) { LogOnce("nocs", "dispatch without a compute program"); break; }
        if (!EnsureStage(shader)) break;
        ShaderStages shaders{};
        shaders[NVN_SHADER_STAGE_COMPUTE] = shader;
        EnsureGlobals(); VkPipeline pipe = BuildCompute(shaders); if (!pipe) break;
        GlobalBarrier(s.cb);
        F.vkCmdBindPipeline(s.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        if (!BindDescriptors(s, shaders, true)) break;
        if (c.op == Op::Dispatch) F.vkCmdDispatch(s.cb, (uint32_t)c.i[0], (uint32_t)c.i[1], (uint32_t)c.i[2]);
        else F.vkCmdDispatchIndirect(s.cb, indirect, offset);
        GlobalBarrier(s.cb); g_stats.dispatches++; break;
      }
      case Op::ClearColor: DoClearColor(s, c); break;
      case Op::ClearDepthStencil: DoClearDepth(s, c); break;
      case Op::CopyBufToBuf: case Op::CopyBufToTex: case Op::CopyTexToBuf: case Op::CopyTexToTex: DoCopy(s, c); break;
      case Op::Downsample: DoDownsample(s, c); break;
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
static std::vector<QueueImpl*> g_queues;       // live queues (guarded by the API lock): QueueScrub walks them

// A pool, program or texture is being finalized: forget it in every queue's saved GPU state.
void QueueScrub(const void* o) {
  for (QueueImpl* q : g_queues) {
    RS* h = q->hw; if (!h) continue;
    if ((void*)h->texPool == o) h->texPool = nullptr;
    if ((void*)h->sampPool == o) h->sampPool = nullptr;
    if ((void*)h->stageOwners[NVN_SHADER_STAGE_VERTEX] == o) h->missingProgramReason = "bound program was finalized";
    for (auto& owner : h->stageOwners) if ((void*)owner == o) owner = nullptr;
    for (uint32_t i = 0; i < 8; i++) if (h->color[i].tex && (void*)h->color[i].tex->impl == o) { h->color[i].tex = nullptr; h->rtDirty = true; }
    if (h->depth.tex && (void*)h->depth.tex->impl == o) { h->depth.tex = nullptr; h->rtDirty = true; }
  }
}

bool QueueCreate(QueueImpl* q) {
  g_queues.push_back(q);
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pi.queueFamilyIndex = g_vk.family;
  return g_vk.f.vkCreateCommandPool(g_vk.dev, &pi, nullptr, &q->pool) == VK_SUCCESS;
}
void QueueDestroy(QueueImpl* q) {
  QueueFinish(q);
  g_queues.erase(std::remove(g_queues.begin(), g_queues.end(), q), g_queues.end());
  delete q->hw; q->hw = nullptr;
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
  if (!q->hw) q->hw = new RS;
  RS& s = *q->hw;                       // GPU state carries over from the previous command buffer, as on the hardware
  s.cb = cb; s.value = value; s.scratch = sc.get(); s.dpools = dp.get(); s.keep = keep.get(); s.q = q; s.dev = q->dev;
  if (q->dev) { s.depthMode = q->dev->depthMode; s.originMode = q->dev->originMode; }
  s.rendering = false; s.rtDirty = true; s.pipeDirty = true; s.curPipe = VK_NULL_HANDLE;    // the Vulkan side starts empty in every submission
  static bool vehDone = false; if (!vehDone) { vehDone = true; AddVectoredExceptionHandler(1, CrumbVeh); }
  g_crumb.active = true; g_crumb.where = "submit"; g_crumb.index = 0; g_crumb.op = -1;
  for (uint32_t i = 0; i < n; i++) {
    auto l = FindList(handles[i]);
    if (!l) { LogOnce("badh", "SubmitCommands with an unknown command handle 0x%llx", (unsigned long long)handles[i]); continue; }
    keep->push_back(l);
    Exec(s, *l, 0);
  }
  g_crumb.where = "submit end";
  EndRendering(s);
  s.cb = VK_NULL_HANDLE; s.scratch = nullptr; s.dpools = nullptr; s.keep = nullptr; s.rendering = false; s.rtDirty = true; s.pipeDirty = true;
  F.vkEndCommandBuffer(cb);
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; cbi.commandBuffer = cb;
  VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; sig.semaphore = g_vk.timeline; sig.value = value; sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi; si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &sig;
  g_crumb.where = "vkQueueSubmit2";
  VkResult r = F.vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
  if (r != VK_SUCCESS) Err("vkQueueSubmit2 failed (%d)", r);
  g_crumb.where = "after submit";
  q->lastSignal = value; g_stats.submits++;
  VkCommandPool pool = q->pool;
  DeferUntil(value, [=]() { FinishSubmission(pool, cb, sc, dp, keep); });
  g_crumb.where = "RunDeferred";
  RunDeferred();
  g_crumb.active = false;
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

static WindowProvider g_provider;
void SetWindowProvider(const WindowProvider* p) { g_provider = p ? *p : WindowProvider{}; }
bool HasWindowProvider() { return g_provider.create_surface != nullptr; }

bool WindowCreate(WindowImpl* w) {
  if (!g_vk.hasSwapchain) return true;                                          // offscreen: the game's textures are the target
  if (g_provider.create_surface) {
    // task-22: the platform layer (SDL3) owns the window; hidden unless the user asked for --window
    if (!g_provider.create_surface(g_provider.user, g_vk.inst, &w->surface) || !w->surface) { w->surface = VK_NULL_HANDLE; Log("window provider made no surface; presenting offscreen only"); return true; }
    w->providerSurface = true;
  } else {
    if (!g_vk.f.vkCreateWin32SurfaceKHR) return true;
    WNDCLASSA wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "SrNvnHidden";
    RegisterClassA(&wc);
    // A real window handle for the surface. Never shown: no ShowWindow, no WS_VISIBLE, created as a tool window off screen.
    w->hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "SrNvnHidden", "nvn-hidden", WS_POPUP, -32000, -32000, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w->hwnd) return true;
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; si.hinstance = wc.hInstance; si.hwnd = w->hwnd;
    if (g_vk.f.vkCreateWin32SurfaceKHR(g_vk.inst, &si, nullptr, &w->surface) != VK_SUCCESS) { w->surface = VK_NULL_HANDLE; return true; }
  }
  VkBool32 sup = VK_FALSE; g_vk.f.vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.family, w->surface, &sup);
  if (!sup) {
    if (w->providerSurface) { if (g_provider.destroy_surface) g_provider.destroy_surface(g_provider.user, g_vk.inst, w->surface); }
    else g_vk.f.vkDestroySurfaceKHR(g_vk.inst, w->surface, nullptr);
    w->surface = VK_NULL_HANDLE; w->providerSurface = false; return true;
  }
  VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  g_vk.f.vkCreateSemaphore(g_vk.dev, &sc, nullptr, &w->acquireSem);
  w->useSwapchain = true;
  return true;
}

void WindowDestroy(WindowImpl* w) {
  if (!g_vk.inited) return;
  g_vk.f.vkDeviceWaitIdle(g_vk.dev);
  for (VkImageView v : w->swapViews) g_vk.f.vkDestroyImageView(g_vk.dev, v, nullptr);
  upscale::Shutdown();
  if (w->swap) g_vk.f.vkDestroySwapchainKHR(g_vk.dev, w->swap, nullptr);
  if (w->acquireSem) g_vk.f.vkDestroySemaphore(g_vk.dev, w->acquireSem, nullptr);
  for (VkSemaphore s : w->presentSems) g_vk.f.vkDestroySemaphore(g_vk.dev, s, nullptr);
  if (w->surface) {
    if (w->providerSurface) { if (g_provider.destroy_surface) g_provider.destroy_surface(g_provider.user, g_vk.inst, w->surface); }
    else g_vk.f.vkDestroySurfaceKHR(g_vk.inst, w->surface, nullptr);
  }
  if (w->hwnd) DestroyWindow(w->hwnd);
  *w = WindowImpl{};
}

// Makes (or remakes) the swapchain for the current surface size. false: nothing to present to right now (minimized, or creation failed).
// `force` rebuilds it even when nothing looks different (after VK_ERROR_OUT_OF_DATE_KHR).
static bool EnsureSwapchain(WindowImpl* w, VkFormat want, uint32_t width, uint32_t height, bool force = false) {
  auto& F = g_vk.f;
  VkSurfaceCapabilitiesKHR caps; F.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, w->surface, &caps);
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) ext = {width, height};
  if (ext.width == 0 || ext.height == 0) return false;                          // minimized: skip the present, keep the swapchain
  ext.width = std::max(caps.minImageExtent.width, std::min(ext.width, caps.maxImageExtent.width));
  ext.height = std::max(caps.minImageExtent.height, std::min(ext.height, caps.maxImageExtent.height));
  bool hidden = g_provider.is_visible ? !g_provider.is_visible(g_provider.user) : false;
  if (!force && w->swap && w->swapFormat == want && w->swapExtent.width == ext.width && w->swapExtent.height == ext.height && w->swapHidden == hidden) return true;
  F.vkDeviceWaitIdle(g_vk.dev);
  uint32_t nf = 0; F.vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, w->surface, &nf, nullptr);
  std::vector<VkSurfaceFormatKHR> fm(nf); F.vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, w->surface, &nf, fm.data());
  VkSurfaceFormatKHR pick = fm.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fm[0];
  for (auto& f : fm) if (f.format == want) pick = f;
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;                              // on screen: vsync
  if (hidden) {                                                                  // nobody sees a hidden window: do not let vsync pace the game
    uint32_t nm = 0; F.vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, w->surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> pm(nm); F.vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, w->surface, &nm, pm.data());
    for (auto m : pm) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
  }
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = w->surface; ci.minImageCount = std::max(2u, caps.minImageCount); ci.imageFormat = pick.format; ci.imageColorSpace = pick.colorSpace; ci.imageExtent = ext;
  ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  // Only when the surface offers it: lets the overlay hook (task-16 hashes) and SMRPG_PRESENT_HASH read the presented image back. Output is unchanged.
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = mode; ci.clipped = VK_TRUE; ci.oldSwapchain = w->swap;
  VkSwapchainKHR ns = VK_NULL_HANDLE;
  if (F.vkCreateSwapchainKHR(g_vk.dev, &ci, nullptr, &ns) != VK_SUCCESS) { LogOnce("swapfail", "swapchain creation failed; presenting offscreen only"); w->useSwapchain = false; return false; }
  if (w->swap) F.vkDestroySwapchainKHR(g_vk.dev, w->swap, nullptr);
  w->swap = ns; w->swapFormat = pick.format; w->swapExtent = ext; w->swapHidden = hidden;
  uint32_t ni = 0; F.vkGetSwapchainImagesKHR(g_vk.dev, ns, &ni, nullptr); w->swapImages.resize(ni); F.vkGetSwapchainImagesKHR(g_vk.dev, ns, &ni, w->swapImages.data());
  for (VkImageView v : w->swapViews) F.vkDestroyImageView(g_vk.dev, v, nullptr);      // the device is idle (waited above)
  w->swapViews.assign(ni, VK_NULL_HANDLE);
  for (uint32_t i = 0; i < ni; i++) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = w->swapImages[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = pick.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    F.vkCreateImageView(g_vk.dev, &vi, nullptr, &w->swapViews[i]);
  }
  w->swapGeneration++;
  for (VkSemaphore s : w->presentSems) F.vkDestroySemaphore(g_vk.dev, s, nullptr);      // the device is idle (waited above)
  w->presentSems.assign(ni, VK_NULL_HANDLE);
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto& s : w->presentSems) F.vkCreateSemaphore(g_vk.dev, &sci, nullptr, &s);
  return true;
}

void QueuePresent(QueueImpl* q, WindowImpl* w, uint32_t idx) {
  w->presents++;
  if (g_provider.on_present) g_provider.on_present(g_provider.user);
  if (w->presents % 120 == 0) Log("stats: %llu presents, %llu draws, %llu skipped (%llu no program, %llu incomplete program), %llu pipelines (%llu failed), %llu submits", (unsigned long long)w->presents, (unsigned long long)g_stats.draws, (unsigned long long)g_stats.skippedDraws, (unsigned long long)g_stats.noProgNull, (unsigned long long)g_stats.noProgIncomplete, (unsigned long long)g_stats.pipelines, (unsigned long long)g_stats.pipelineFails, (unsigned long long)g_stats.submits);
  if (!w->useSwapchain || idx >= w->textures.size() || !w->textures[idx] || !w->textures[idx]->impl) return;
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  auto& F = g_vk.f;
  TextureImpl* t = w->textures[idx]->impl;
  bool srgb = t->vkFormat == VK_FORMAT_R8G8B8A8_SRGB || t->vkFormat == VK_FORMAT_B8G8R8A8_SRGB;
  VkFormat swapWant = srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
  if (!EnsureSwapchain(w, swapWant, t->width, t->height)) return;
  uint32_t img = 0;
  VkResult r = F.vkAcquireNextImageKHR(g_vk.dev, w->swap, UINT64_MAX, w->acquireSem, VK_NULL_HANDLE, &img);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {                                           // the window changed under the swapchain: rebuild once and retry
    if (!EnsureSwapchain(w, swapWant, t->width, t->height, true)) return;
    r = F.vkAcquireNextImageKHR(g_vk.dev, w->swap, UINT64_MAX, w->acquireSem, VK_NULL_HANDLE, &img);
  }
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
  // Frame order (task-23): 1. game image to the swapchain image, through the upscale pass when one is on, else the plain blit;
  //                        2. PlatOverlayDraw (task-16); 3. present. Hook 1 returns false with upscale off, and then nothing changes.
  const bool flipY = q->dev && q->dev->originMode == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
  upscale::PresentIO uio{cb, t->image, t->vkFormat, t->width, t->height, flipY, w->swapImages[img], w->swapFormat, w->swapExtent.width, w->swapExtent.height, w->presents};
  if (!upscale::Record(uio)) {
    VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1] = {(int32_t)t->width, (int32_t)t->height, 1};
    if (flipY) { bl.srcOffsets[0].y = (int32_t)t->height; bl.srcOffsets[1].y = 0; } bl.dstOffsets[1] = {(int32_t)w->swapExtent.width, (int32_t)w->swapExtent.height, 1};
    F.vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, w->swapImages[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
  }
  trans(w->swapImages[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  {
    PlatOverlayFrame of{};
    of.struct_size = sizeof(of); of.frame = (uint32_t)w->presents;
    of.instance = g_vk.inst; of.physical_device = g_vk.phys; of.device = g_vk.dev; of.queue = g_vk.queue; of.queue_family = g_vk.family;
    of.image_count = (uint32_t)w->swapImages.size(); of.get_instance_proc_addr = (void*)g_vk.f.vkGetInstanceProcAddr;
    of.command_buffer = cb; of.image = w->swapImages[img]; of.image_view = w->swapViews[img]; of.image_index = img; of.format = (uint32_t)w->swapFormat;
    of.width = w->swapExtent.width; of.height = w->swapExtent.height; of.layout = (uint32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    of.game_width = t->width; of.game_height = t->height; of.swapchain_generation = w->swapGeneration;
    PlatOverlayDraw(&of);
  }
  upscale::DebugAfterOverlay(uio);                                  // hashes the presented image when SMRPG_PRESENT_HASH asks; otherwise returns at once
  trans(w->swapImages[img], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
