// Star Road Recomp: NVN API, memory pools, buffers, textures, samplers and their pools (task-12). Our own code (D-008).
#include <algorithm>

#include "../gfx/texture/texture.h"
#include "nvn_api.h"

namespace nvn {

static_assert(sizeof(MemPoolBuilderG) <= kSizeMemoryPoolBuilder, "memory pool builder size");
static_assert(sizeof(BufferBuilderG) <= kSizeBufferBuilder, "buffer builder size");
static_assert(sizeof(SamplerBuilderG) <= kSizeSamplerBuilder, "sampler builder size");
static_assert(sizeof(TextureBuilderG) <= kSizeTextureBuilder, "texture builder size");
static_assert(sizeof(ObjHdr) <= kSizeTexturePool && sizeof(ObjHdr) <= kSizeSamplerPool && sizeof(ObjHdr) <= kSizeBuffer, "object header fits");

static void DirtyRange(Pool* p, uint64_t off, uint64_t size) {
  if (!p) return;
  for (TextureImpl* t : p->textures) {
    uint64_t a = t->poolOffset, b = a + std::max<uint64_t>(t->storageSize, 1);
    if (off < b && off + size > a) t->uploaded = false;       // the guest rewrote texel memory: upload again at the next sample
  }
  p->gen++;
}

// ---- memory pool ---------------------------------------------------------------------------------------------------
NVN_API(nvnMemoryPoolBuilderSetDefaults, void, (MemPoolBuilderG* b)) { void* d = b->dev; memset(b, 0, sizeof *b); b->dev = d; b->flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED; }
NVN_API(nvnMemoryPoolBuilderSetDevice, void, (MemPoolBuilderG* b, void* dev)) { b->dev = dev; }
NVN_API(nvnMemoryPoolBuilderSetFlags, void, (MemPoolBuilderG* b, int flags)) { b->flags = (uint32_t)flags; }
NVN_API(nvnMemoryPoolBuilderSetStorage, void, (MemPoolBuilderG* b, void* memory, size_t size)) { b->memory = memory; b->size = size; }
NVN_API(nvnMemoryPoolBuilderGetDevice, void*, (const MemPoolBuilderG* b)) { return b->dev; }
NVN_API(nvnMemoryPoolBuilderGetFlags, int, (const MemPoolBuilderG* b)) { return (int)b->flags; }
NVN_API(nvnMemoryPoolBuilderGetMemory, void*, (const MemPoolBuilderG* b)) { return b->memory; }
NVN_API(nvnMemoryPoolBuilderGetSize, size_t, (const MemPoolBuilderG* b)) { return b->size; }

NVN_API(nvnMemoryPoolInitialize, bool, (void* pool, const MemPoolBuilderG* b)) {
  Pool* p = new Pool; p->flags = b->flags; p->memory = (uint8_t*)b->memory; p->size = b->size;
  if (!PoolCreate(p)) { delete p; return false; }
  SetHdr(pool, kMagicPool, p);
  return true;
}
NVN_API(nvnMemoryPoolFinalize, void, (void* pool)) { Pool* p = ImplOf<Pool>(pool); if (!p) return; PoolDestroy(p); delete p; ClearHdr(pool); }
NVN_API(nvnMemoryPoolSetDebugLabel, void, (void* pool, const char* l)) { if (Pool* p = ImplOf<Pool>(pool)) p->label = l ? l : ""; }
NVN_API(nvnMemoryPoolMap, void*, (const void* pool)) { const Pool* p = ImplOf<Pool>(pool); return p ? p->memory : nullptr; }
NVN_API(nvnMemoryPoolFlushMappedRange, void, (const void* pool, int64_t off, size_t size)) { DirtyRange(ImplOf<Pool>(pool), (uint64_t)off, size); }
NVN_API(nvnMemoryPoolInvalidateMappedRange, void, (const void* pool, int64_t off, size_t size)) { (void)pool; (void)off; (void)size; }   // host memory is imported: always coherent
NVN_API(nvnMemoryPoolGetBufferAddress, uint64_t, (const void* pool)) { const Pool* p = ImplOf<Pool>(pool); return p ? p->va : 0; }
NVN_API(nvnMemoryPoolGetFlags, int, (const void* pool)) { const Pool* p = ImplOf<Pool>(pool); return p ? (int)p->flags : 0; }
NVN_API(nvnMemoryPoolGetSize, size_t, (const void* pool)) { const Pool* p = ImplOf<Pool>(pool); return p ? p->size : 0; }

// ---- buffer ----------------------------------------------------------------------------------------------------------
NVN_API(nvnBufferBuilderSetDefaults, void, (BufferBuilderG* b)) { void* d = b->dev; memset(b, 0, sizeof *b); b->dev = d; }
NVN_API(nvnBufferBuilderSetDevice, void, (BufferBuilderG* b, void* dev)) { b->dev = dev; }
NVN_API(nvnBufferBuilderSetStorage, void, (BufferBuilderG* b, void* pool, int64_t off, int64_t size)) { b->pool = pool; b->offset = (uint64_t)off; b->size = (uint64_t)size; }
NVN_API(nvnBufferBuilderGetDevice, void*, (const BufferBuilderG* b)) { return b->dev; }
NVN_API(nvnBufferBuilderGetMemoryPool, void*, (const BufferBuilderG* b)) { return b->pool; }
NVN_API(nvnBufferBuilderGetMemoryOffset, int64_t, (const BufferBuilderG* b)) { return (int64_t)b->offset; }
NVN_API(nvnBufferBuilderGetSize, int64_t, (const BufferBuilderG* b)) { return (int64_t)b->size; }

NVN_API(nvnBufferInitialize, bool, (void* buffer, const BufferBuilderG* b)) {
  Pool* p = ImplOf<Pool>(b->pool);
  if (!p) { Err("BufferInitialize: builder has no memory pool"); return false; }
  if (b->offset + b->size > p->size) { Err("BufferInitialize: range 0x%llx+0x%llx is outside the pool (size 0x%llx)", (unsigned long long)b->offset, (unsigned long long)b->size, (unsigned long long)p->size); return false; }
  BufImpl* i = new BufImpl; i->pool = p; i->guestPool = b->pool; i->offset = b->offset; i->size = b->size;
  SetHdr(buffer, kMagicBuffer, i);
  return true;
}
NVN_API(nvnBufferFinalize, void, (void* buffer)) { delete ImplOf<BufImpl>(buffer); ClearHdr(buffer); }
NVN_API(nvnBufferSetDebugLabel, void, (void* buffer, const char* l)) { (void)buffer; (void)l; }
NVN_API(nvnBufferMap, void*, (const void* buffer)) { const BufImpl* i = ImplOf<BufImpl>(buffer); return i && i->pool ? i->pool->memory + i->offset : nullptr; }
NVN_API(nvnBufferGetAddress, uint64_t, (const void* buffer)) { const BufImpl* i = ImplOf<BufImpl>(buffer); return i && i->pool ? i->pool->va + i->offset : 0; }
NVN_API(nvnBufferGetSize, int64_t, (const void* buffer)) { const BufImpl* i = ImplOf<BufImpl>(buffer); return i ? (int64_t)i->size : 0; }
NVN_API(nvnBufferGetMemoryOffset, int64_t, (const void* buffer)) { const BufImpl* i = ImplOf<BufImpl>(buffer); return i ? (int64_t)i->offset : 0; }
NVN_API(nvnBufferGetMemoryPool, void*, (const void* buffer)) { const BufImpl* i = ImplOf<BufImpl>(buffer); return i ? i->guestPool : nullptr; }
NVN_API(nvnBufferGetDebugID, uint64_t, (const void* buffer)) { const BufImpl* i = ImplOf<BufImpl>(buffer); return i ? i->debugId : 0; }
NVN_API(nvnBufferFlushMappedRange, void, (const void* buffer, int64_t off, size_t size)) {
  const BufImpl* i = ImplOf<BufImpl>(buffer);
  if (i) DirtyRange(i->pool, i->offset + (uint64_t)off, size);
}
NVN_API(nvnBufferInvalidateMappedRange, void, (const void* buffer, int64_t off, size_t size)) { (void)buffer; (void)off; (void)size; }

// ---- texture builder -----------------------------------------------------------------------------------------------
static void BuilderDefaults(TextureBuilderG* b) {
  void* d = b->dev; memset(b, 0, sizeof *b); b->dev = d;
  b->target = NVN_TEXTURE_TARGET_2D; b->width = b->height = b->depth = 1; b->levels = 1; b->samples = 0;
  b->swz[0] = NVN_TEXTURE_SWIZZLE_R; b->swz[1] = NVN_TEXTURE_SWIZZLE_G; b->swz[2] = NVN_TEXTURE_SWIZZLE_B; b->swz[3] = NVN_TEXTURE_SWIZZLE_A;
}

// Number of array layers a target implies (mirrors TextureCreate and the replay upload).
static void Dims(uint32_t target, uint32_t depth, uint32_t* layers, uint32_t* slices, bool* cube, bool* is3d) {
  *layers = 1; *slices = 1; *cube = false; *is3d = false;
  switch (target) {
    case NVN_TEXTURE_TARGET_1D_ARRAY: case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: *layers = std::max(1u, depth); break;
    case NVN_TEXTURE_TARGET_3D: *slices = std::max(1u, depth); *is3d = true; break;
    case NVN_TEXTURE_TARGET_CUBEMAP: *layers = 6; *cube = true; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: *layers = std::max(1u, depth); if (*layers % 6) *layers *= 6; *cube = true; break;
    default: break;
  }
}

static uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Swizzled storage a texture needs. Uses the task-11 layout model; formats it does not know get a conservative linear estimate.
static uint64_t StorageSizeFor(const TextureBuilderG& b) {
  uint32_t layers, slices; bool cube, is3d; Dims(b.target, b.depth, &layers, &slices, &cube, &is3d);
  uint32_t w = std::max(1u, b.width), h = b.target == NVN_TEXTURE_TARGET_1D || b.target == NVN_TEXTURE_TARGET_1D_ARRAY ? 1u : std::max(1u, b.height);
  uint32_t levels = std::max(1u, b.levels);
  sr_tex_format fmt;
  if (sr_tex_format_from_nvn(b.format, &fmt) == SR_TEX_OK) {
    sr_tex_surface_desc d{};
    d.format = fmt; d.width = w; d.height = h; d.depth = slices; d.layers = layers; d.levels = levels;
    d.dim = is3d ? SR_TEX_DIM_3D : cube ? SR_TEX_DIM_CUBE : SR_TEX_DIM_2D; d.block_height_log2 = SR_TEX_BH_AUTO; d.block_depth_log2 = SR_TEX_BH_AUTO;
    sr_tex_plan plan;
    if (sr_tex_plan_build(&d, &plan) == SR_TEX_OK) return AlignUp(plan.total_size, 0x1000);
  }
  uint64_t bpp = std::max(1u, NvnFormatBytes(b.format)), total = 0;
  for (uint32_t l = 0; l < levels; l++) total += (uint64_t)std::max(1u, w >> l) * std::max(1u, h >> l) * std::max(1u, slices >> l) * bpp;
  return AlignUp(total * layers + 0x1000, 0x1000);
}

NVN_API(nvnTextureBuilderSetDefaults, void, (TextureBuilderG* b)) { BuilderDefaults(b); }
NVN_API(nvnTextureBuilderSetDevice, void, (TextureBuilderG* b, void* dev)) { b->dev = dev; }
NVN_API(nvnTextureBuilderSetFlags, void, (TextureBuilderG* b, int f)) { b->flags = (uint32_t)f; }
NVN_API(nvnTextureBuilderSetTarget, void, (TextureBuilderG* b, int t)) { b->target = (uint32_t)t; }
NVN_API(nvnTextureBuilderSetWidth, void, (TextureBuilderG* b, int v)) { b->width = (uint32_t)v; }
NVN_API(nvnTextureBuilderSetHeight, void, (TextureBuilderG* b, int v)) { b->height = (uint32_t)v; }
NVN_API(nvnTextureBuilderSetDepth, void, (TextureBuilderG* b, int v)) { b->depth = (uint32_t)v; }
NVN_API(nvnTextureBuilderSetSize1D, void, (TextureBuilderG* b, int w)) { b->width = (uint32_t)w; b->height = 1; b->depth = 1; }
NVN_API(nvnTextureBuilderSetSize2D, void, (TextureBuilderG* b, int w, int h)) { b->width = (uint32_t)w; b->height = (uint32_t)h; b->depth = 1; }
NVN_API(nvnTextureBuilderSetSize3D, void, (TextureBuilderG* b, int w, int h, int d)) { b->width = (uint32_t)w; b->height = (uint32_t)h; b->depth = (uint32_t)d; }
NVN_API(nvnTextureBuilderSetLevels, void, (TextureBuilderG* b, int v)) { b->levels = (uint32_t)v; }
NVN_API(nvnTextureBuilderSetFormat, void, (TextureBuilderG* b, int f)) { b->format = (uint32_t)f; }
NVN_API(nvnTextureBuilderSetSamples, void, (TextureBuilderG* b, int v)) { b->samples = (uint32_t)v; }
NVN_API(nvnTextureBuilderSetSwizzle, void, (TextureBuilderG* b, int r, int g, int bl, int a)) { b->swz[0] = (uint32_t)r; b->swz[1] = (uint32_t)g; b->swz[2] = (uint32_t)bl; b->swz[3] = (uint32_t)a; }
NVN_API(nvnTextureBuilderSetDepthStencilMode, void, (TextureBuilderG* b, int m)) { b->dsMode = (uint32_t)m; }
NVN_API(nvnTextureBuilderSetStride, void, (TextureBuilderG* b, int64_t s)) { b->stride = (uint64_t)s; }
NVN_API(nvnTextureBuilderSetStorage, void, (TextureBuilderG* b, void* pool, int64_t off)) { b->pool = pool; b->offset = (uint64_t)off; }
NVN_API(nvnTextureBuilderSetPackagedTextureLayout, void, (TextureBuilderG* b, const void* layout)) {
  if (layout) memcpy(b->packaged, layout, sizeof b->packaged);
  LogOnce("packaged", "TextureBuilderSetPackagedTextureLayout: layout bits are stored but block height comes from the default rule");
}
NVN_API(nvnTextureBuilderGetTarget, int, (const TextureBuilderG* b)) { return (int)b->target; }
NVN_API(nvnTextureBuilderGetWidth, int, (const TextureBuilderG* b)) { return (int)b->width; }
NVN_API(nvnTextureBuilderGetHeight, int, (const TextureBuilderG* b)) { return (int)b->height; }
NVN_API(nvnTextureBuilderGetDepth, int, (const TextureBuilderG* b)) { return (int)b->depth; }
NVN_API(nvnTextureBuilderGetLevels, int, (const TextureBuilderG* b)) { return (int)b->levels; }
NVN_API(nvnTextureBuilderGetFormat, int, (const TextureBuilderG* b)) { return (int)b->format; }
NVN_API(nvnTextureBuilderGetSamples, int, (const TextureBuilderG* b)) { return (int)b->samples; }
NVN_API(nvnTextureBuilderGetFlags, int, (const TextureBuilderG* b)) { return (int)b->flags; }
NVN_API(nvnTextureBuilderGetDepthStencilMode, int, (const TextureBuilderG* b)) { return (int)b->dsMode; }
NVN_API(nvnTextureBuilderGetStride, int64_t, (const TextureBuilderG* b)) { return (int64_t)b->stride; }
NVN_API(nvnTextureBuilderGetDevice, void*, (const TextureBuilderG* b)) { return b->dev; }
NVN_API(nvnTextureBuilderGetMemoryPool, void*, (const TextureBuilderG* b)) { return b->pool; }
NVN_API(nvnTextureBuilderGetMemoryOffset, int64_t, (const TextureBuilderG* b)) { return (int64_t)b->offset; }
NVN_API(nvnTextureBuilderGetStorageSize, int64_t, (const TextureBuilderG* b)) { return (int64_t)StorageSizeFor(*b); }
NVN_API(nvnTextureBuilderGetStorageAlignment, int64_t, (const TextureBuilderG* b)) { (void)b; return 0x1000; }
NVN_API(nvnTextureBuilderGetZCullStorageSize, int64_t, (const TextureBuilderG* b)) { (void)b; return 0; }
NVN_API(nvnTextureBuilderGetSwizzle, void, (const TextureBuilderG* b, int* r, int* g, int* bl, int* a)) { *r = (int)b->swz[0]; *g = (int)b->swz[1]; *bl = (int)b->swz[2]; *a = (int)b->swz[3]; }

// ---- texture -------------------------------------------------------------------------------------------------------
NVN_API(nvnTextureInitialize, bool, (void* tex, const TextureBuilderG* b)) {
  TextureG* g = (TextureG*)tex;
  Pool* p = ImplOf<Pool>(b->pool);
  if (!p) { Err("TextureInitialize: builder has no memory pool"); return false; }
  memset(g, 0, kSizeTexture);
  g->magic = kMagicTexture; g->target = b->target; g->format = b->format; g->width = b->width; g->height = b->height; g->depth = b->depth;
  g->levels = std::max(1u, b->levels); g->samples = b->samples; g->flags = b->flags; g->dsMode = b->dsMode;
  for (int i = 0; i < 4; i++) g->swz[i] = b->swz[i];
  g->stride = b->stride; g->pool = b->pool; g->memOffset = b->offset;
  g->storageSize = StorageSizeFor(*b); g->zcullSize = 0; g->textureAddr = p->va + b->offset;
  TextureImpl* t = new TextureImpl;
  t->pool = p; t->poolOffset = b->offset; t->storageSize = g->storageSize;
  if (!TextureCreate(g, t)) { delete t; memset(g, 0, kSizeTexture); return false; }
  g->impl = t;
  p->textures.push_back(t);
  return true;
}
NVN_API(nvnTextureFinalize, void, (void* tex)) {
  TextureG* g = (TextureG*)tex;
  if (!g || !g->impl) return;
  TextureImpl* t = g->impl;
  if (t->pool) { auto& v = t->pool->textures; v.erase(std::remove(v.begin(), v.end(), t), v.end()); }
  TextureDestroy(t);
  g->impl = nullptr; g->magic = 0;
  // The impl struct is released with the deferred destroy of its Vulkan objects (it keeps no references afterwards).
  delete t;
}
NVN_API(nvnTextureSetDebugLabel, void, (void* tex, const char* l)) { (void)tex; (void)l; }
NVN_API(nvnTextureGetWidth, int, (const TextureG* t)) { return (int)t->width; }
NVN_API(nvnTextureGetHeight, int, (const TextureG* t)) { return (int)t->height; }
NVN_API(nvnTextureGetDepth, int, (const TextureG* t)) { return (int)t->depth; }
NVN_API(nvnTextureGetLevels, int, (const TextureG* t)) { return (int)t->levels; }
NVN_API(nvnTextureGetTarget, int, (const TextureG* t)) { return (int)t->target; }
NVN_API(nvnTextureGetFormat, int, (const TextureG* t)) { return (int)t->format; }
NVN_API(nvnTextureGetSamples, int, (const TextureG* t)) { return (int)t->samples; }
NVN_API(nvnTextureGetFlags, int, (const TextureG* t)) { return (int)t->flags; }
NVN_API(nvnTextureGetDepthStencilMode, int, (const TextureG* t)) { return (int)t->dsMode; }
NVN_API(nvnTextureGetStride, int64_t, (const TextureG* t)) { return (int64_t)t->stride; }
NVN_API(nvnTextureGetStorageSize, int64_t, (const TextureG* t)) { return (int64_t)t->storageSize; }
NVN_API(nvnTextureGetZCullStorageSize, int64_t, (const TextureG* t)) { return (int64_t)t->zcullSize; }
NVN_API(nvnTextureGetMemoryOffset, int64_t, (const TextureG* t)) { return (int64_t)t->memOffset; }
NVN_API(nvnTextureGetMemoryPool, void*, (const TextureG* t)) { return t->pool; }
NVN_API(nvnTextureGetTextureAddress, uint64_t, (const TextureG* t)) { return t->textureAddr; }
NVN_API(nvnTextureGetDebugID, uint64_t, (const TextureG* t)) { return t->debug; }
NVN_API(nvnTextureGetSwizzle, void, (const TextureG* t, int* r, int* g, int* b, int* a)) { *r = (int)t->swz[0]; *g = (int)t->swz[1]; *b = (int)t->swz[2]; *a = (int)t->swz[3]; }

// Texel transfer: deswizzle the level, patch the region, swizzle back (write); or deswizzle and copy out (read).
static void TransferTexels(TextureG* g, const TextureView* v, const CopyRegion* r, void* data, bool write) {
  TextureImpl* t = g->impl;
  if (!t || !t->pool || !t->pool->memory || !r || !data) return;
  sr_tex_format fmt;
  if (sr_tex_format_from_nvn(g->format, &fmt) != SR_TEX_OK) { LogOnce("xferfmt", "Texture%sTexels: format 0x%x not handled", write ? "Write" : "Read", g->format); return; }
  const sr_tex_format_info* fi = sr_tex_format_info_get(fmt.fmt);
  if (!fi) return;
  uint32_t layers, slices; bool cube, is3d; Dims(g->target, g->depth, &layers, &slices, &cube, &is3d);
  sr_tex_surface_desc d{};
  d.format = fmt; d.width = t->width; d.height = t->height; d.depth = slices; d.layers = layers; d.levels = t->levels;
  d.dim = is3d ? SR_TEX_DIM_3D : cube ? SR_TEX_DIM_CUBE : SR_TEX_DIM_2D; d.block_height_log2 = SR_TEX_BH_AUTO; d.block_depth_log2 = SR_TEX_BH_AUTO;
  sr_tex_plan plan;
  if (sr_tex_plan_build(&d, &plan) != SR_TEX_OK) return;
  uint32_t level = v && (v->flags & 8) ? v->minLevel : 0, layer0 = v && (v->flags & 16) ? v->minLayer : 0;
  if (level >= t->levels) return;
  const sr_tex_level_plan& lp = plan.level[level];
  uint8_t* swz = t->pool->memory + t->poolOffset;
  size_t swzSize = (size_t)std::min<uint64_t>(t->pool->size - t->poolOffset, plan.total_size);
  uint32_t bw = fi->block_w ? fi->block_w : 1, bh = fi->block_h ? fi->block_h : 1, bpb = fi->bytes_per_block;
  uint32_t bx0 = (uint32_t)r->x / bw, by0 = (uint32_t)r->y / bh;
  uint32_t nbx = ((uint32_t)r->w + bw - 1) / bw, nby = ((uint32_t)r->h + bh - 1) / bh;
  uint32_t nz = (uint32_t)std::max(1, r->d);
  if (bx0 + nbx > lp.blocks_w || by0 + nby > lp.blocks_h) { LogOnce("xferrange", "Texture%sTexels: region outside the level", write ? "Write" : "Read"); return; }
  uint64_t rowBytes = (uint64_t)lp.blocks_w * bpb, sliceBytes = rowBytes * lp.blocks_h;
  uint8_t* bytes = (uint8_t*)data;
  uint64_t outRow = (uint64_t)nbx * bpb, outSlice = outRow * nby;
  std::vector<uint8_t> lin;
  auto doOne = [&](uint32_t layer, uint32_t zFirst, uint32_t zCount, uint8_t* io) {
    lin.assign((size_t)lp.linear_size, 0);
    if (sr_tex_deswizzle_level(&plan, layer, level, swz, swzSize, lin.data(), 0, 0) != SR_TEX_OK) return;
    for (uint32_t z = 0; z < zCount; z++)
      for (uint32_t y = 0; y < nby; y++) {
        uint8_t* l = lin.data() + (uint64_t)(zFirst + z) * sliceBytes + (uint64_t)(by0 + y) * rowBytes + (uint64_t)bx0 * bpb;
        uint8_t* u = io + z * outSlice + y * outRow;
        if (write) memcpy(l, u, (size_t)outRow); else memcpy(u, l, (size_t)outRow);
      }
    if (write) sr_tex_swizzle_level(&plan, layer, level, lin.data(), 0, 0, swz, swzSize);
  };
  if (is3d) doOne(0, (uint32_t)r->z, nz, bytes);
  else for (uint32_t k = 0; k < nz; k++) doOne(layer0 + (uint32_t)r->z + k, 0, 1, bytes + k * outSlice);
  if (write) t->uploaded = false;
}
NVN_API(nvnTextureWriteTexels, void, (void* tex, const TextureView* v, const CopyRegion* r, const void* data)) { TransferTexels((TextureG*)tex, v, r, (void*)data, true); }
NVN_API(nvnTextureReadTexels, void, (const void* tex, const TextureView* v, const CopyRegion* r, void* data)) { TransferTexels((TextureG*)tex, v, r, data, false); }
NVN_API(nvnTextureFlushTexels, void, (const void* tex, const TextureView* v, const CopyRegion* r)) { (void)v; (void)r; TextureImpl* t = ((const TextureG*)tex)->impl; if (t) t->uploaded = false; }
NVN_API(nvnTextureInvalidateTexels, void, (const void* tex, const TextureView* v, const CopyRegion* r)) { (void)tex; (void)v; (void)r; }

// ---- texture view (16 bytes in place) ----------------------------------------------------------------------------------
NVN_API(nvnTextureViewSetDefaults, void, (TextureView* v)) { memset(v, 0, sizeof *v); }
NVN_API(nvnTextureViewSetTarget, void, (TextureView* v, int t)) { v->flags |= 1; v->target = (uint8_t)t; }
NVN_API(nvnTextureViewSetFormat, void, (TextureView* v, int f)) { v->flags |= 2; v->format = (uint8_t)f; }
NVN_API(nvnTextureViewSetSwizzle, void, (TextureView* v, int r, int g, int b, int a)) { v->flags |= 4; v->swz[0] = (uint8_t)r; v->swz[1] = (uint8_t)g; v->swz[2] = (uint8_t)b; v->swz[3] = (uint8_t)a; }
NVN_API(nvnTextureViewSetLevels, void, (TextureView* v, int base, int n)) { v->flags |= 8; v->minLevel = (uint16_t)base; v->numLevels = (uint16_t)n; }
NVN_API(nvnTextureViewSetLayers, void, (TextureView* v, int base, int n)) { v->flags |= 16; v->minLayer = (uint16_t)base; v->numLayers = (uint16_t)n; }
NVN_API(nvnTextureViewSetDepthStencilMode, void, (TextureView* v, int m)) { (void)v; (void)m; LogOnce("viewds", "TextureViewSetDepthStencilMode is ignored (depth is sampled)"); }
NVN_API(nvnTextureViewGetTarget, bool, (const TextureView* v, int* t)) { if (!(v->flags & 1)) return false; *t = v->target; return true; }
NVN_API(nvnTextureViewGetFormat, bool, (const TextureView* v, int* f)) { if (!(v->flags & 2)) return false; *f = v->format; return true; }
NVN_API(nvnTextureViewGetSwizzle, bool, (const TextureView* v, int* r, int* g, int* b, int* a)) {
  if (!(v->flags & 4)) return false;
  *r = v->swz[0]; *g = v->swz[1]; *b = v->swz[2]; *a = v->swz[3]; return true;
}
NVN_API(nvnTextureViewGetLevels, bool, (const TextureView* v, int* base, int* n)) { if (!(v->flags & 8)) return false; *base = v->minLevel; *n = v->numLevels; return true; }
NVN_API(nvnTextureViewGetLayers, bool, (const TextureView* v, int* base, int* n)) { if (!(v->flags & 16)) return false; *base = v->minLayer; *n = v->numLayers; return true; }
NVN_API(nvnTextureViewCompare, bool, (const TextureView* a, const TextureView* b)) { return memcmp(a, b, sizeof *a) == 0; }
NVN_API(nvnTextureCompare, bool, (const TextureG* a, const TextureG* b)) { return a == b || (a->impl && a->impl == b->impl); }

// ---- sampler builder ---------------------------------------------------------------------------------------------------
static void SamplerDefaults(SamplerBuilderG* b) {
  void* d = b->dev; memset(b, 0, sizeof *b); b->dev = d;
  b->minFilter = NVN_MIN_FILTER_NEAREST; b->magFilter = NVN_MAG_FILTER_NEAREST;
  b->wrapS = b->wrapT = b->wrapR = NVN_WRAP_MODE_REPEAT;
  b->compareMode = NVN_COMPARE_MODE_NONE; b->compareFunc = NVN_COMPARE_FUNC_ALWAYS; b->reduction = NVN_SAMPLER_REDUCTION_AVERAGE;
  b->lodBias = 0; b->minLod = 0; b->maxLod = 1000.0f; b->maxAniso = 1.0f;
}
NVN_API(nvnSamplerBuilderSetDefaults, void, (SamplerBuilderG* b)) { SamplerDefaults(b); }
NVN_API(nvnSamplerBuilderSetDevice, void, (SamplerBuilderG* b, void* dev)) { b->dev = dev; }
NVN_API(nvnSamplerBuilderSetMinMagFilter, void, (SamplerBuilderG* b, int mn, int mg)) { b->minFilter = (uint32_t)mn; b->magFilter = (uint32_t)mg; }
NVN_API(nvnSamplerBuilderSetWrapMode, void, (SamplerBuilderG* b, int s, int t, int r)) { b->wrapS = (uint32_t)s; b->wrapT = (uint32_t)t; b->wrapR = (uint32_t)r; }
NVN_API(nvnSamplerBuilderSetLodClamp, void, (SamplerBuilderG* b, float mn, float mx)) { b->minLod = mn; b->maxLod = mx; }
NVN_API(nvnSamplerBuilderSetLodBias, void, (SamplerBuilderG* b, float v)) { b->lodBias = v; }
NVN_API(nvnSamplerBuilderSetLodSnap, void, (SamplerBuilderG* b, float v)) { (void)b; (void)v; }
NVN_API(nvnSamplerBuilderSetCompare, void, (SamplerBuilderG* b, int mode, int func)) { b->compareMode = (uint32_t)mode; b->compareFunc = (uint32_t)func; }
NVN_API(nvnSamplerBuilderSetBorderColor, void, (SamplerBuilderG* b, const float* c)) { for (int i = 0; i < 4; i++) b->border[i] = c[i]; }
NVN_API(nvnSamplerBuilderSetBorderColori, void, (SamplerBuilderG* b, const int32_t* c)) { for (int i = 0; i < 4; i++) b->border[i] = (float)c[i]; }
NVN_API(nvnSamplerBuilderSetBorderColorui, void, (SamplerBuilderG* b, const uint32_t* c)) { for (int i = 0; i < 4; i++) b->border[i] = (float)c[i]; }
NVN_API(nvnSamplerBuilderSetMaxAnisotropy, void, (SamplerBuilderG* b, float v)) { b->maxAniso = v; }
NVN_API(nvnSamplerBuilderSetReductionFilter, void, (SamplerBuilderG* b, int v)) { b->reduction = (uint32_t)v; }
NVN_API(nvnSamplerBuilderGetDevice, void*, (const SamplerBuilderG* b)) { return b->dev; }
NVN_API(nvnSamplerBuilderGetMinMagFilter, void, (const SamplerBuilderG* b, int* mn, int* mg)) { *mn = (int)b->minFilter; *mg = (int)b->magFilter; }
NVN_API(nvnSamplerBuilderGetWrapMode, void, (const SamplerBuilderG* b, int* s, int* t, int* r)) { *s = (int)b->wrapS; *t = (int)b->wrapT; *r = (int)b->wrapR; }
NVN_API(nvnSamplerBuilderGetLodClamp, void, (const SamplerBuilderG* b, float* mn, float* mx)) { *mn = b->minLod; *mx = b->maxLod; }
NVN_API(nvnSamplerBuilderGetLodBias, float, (const SamplerBuilderG* b)) { return b->lodBias; }
NVN_API(nvnSamplerBuilderGetCompare, void, (const SamplerBuilderG* b, int* mode, int* func)) { *mode = (int)b->compareMode; *func = (int)b->compareFunc; }
NVN_API(nvnSamplerBuilderGetBorderColor, void, (const SamplerBuilderG* b, float* c)) { for (int i = 0; i < 4; i++) c[i] = b->border[i]; }
NVN_API(nvnSamplerBuilderGetBorderColori, void, (const SamplerBuilderG* b, int32_t* c)) { for (int i = 0; i < 4; i++) c[i] = (int32_t)b->border[i]; }
NVN_API(nvnSamplerBuilderGetBorderColorui, void, (const SamplerBuilderG* b, uint32_t* c)) { for (int i = 0; i < 4; i++) c[i] = (uint32_t)b->border[i]; }
NVN_API(nvnSamplerBuilderGetMaxAnisotropy, float, (const SamplerBuilderG* b)) { return b->maxAniso; }
NVN_API(nvnSamplerBuilderGetReductionFilter, int, (const SamplerBuilderG* b)) { return (int)b->reduction; }

// ---- sampler -------------------------------------------------------------------------------------------------------------
NVN_API(nvnSamplerInitialize, bool, (void* samp, const SamplerBuilderG* b)) {
  SamplerImpl* s = new SamplerImpl; s->desc = *b;
  if (!SamplerCreate(s)) { delete s; return false; }
  SetHdr(samp, kMagicSampler, s);
  return true;
}
NVN_API(nvnSamplerFinalize, void, (void* samp)) { SamplerImpl* s = ImplOf<SamplerImpl>(samp); if (!s) return; SamplerDestroy(s); delete s; ClearHdr(samp); }
NVN_API(nvnSamplerSetDebugLabel, void, (void* samp, const char* l)) { (void)samp; (void)l; }
NVN_API(nvnSamplerGetDebugID, uint64_t, (const void* samp)) { return (uint64_t)(uintptr_t)ImplOf<SamplerImpl>(samp); }
NVN_API(nvnSamplerCompare, bool, (const void* a, const void* b)) { return ImplOf<SamplerImpl>(a) == ImplOf<SamplerImpl>(b); }
NVN_API(nvnSamplerGetMinMagFilter, void, (const void* s, int* mn, int* mg)) { const SamplerImpl* i = ImplOf<SamplerImpl>(s); *mn = (int)i->desc.minFilter; *mg = (int)i->desc.magFilter; }
NVN_API(nvnSamplerGetWrapMode, void, (const void* s, int* a, int* b, int* c)) { const SamplerImpl* i = ImplOf<SamplerImpl>(s); *a = (int)i->desc.wrapS; *b = (int)i->desc.wrapT; *c = (int)i->desc.wrapR; }
NVN_API(nvnSamplerGetLodClamp, void, (const void* s, float* mn, float* mx)) { const SamplerImpl* i = ImplOf<SamplerImpl>(s); *mn = i->desc.minLod; *mx = i->desc.maxLod; }
NVN_API(nvnSamplerGetLodBias, float, (const void* s)) { return ImplOf<SamplerImpl>(s)->desc.lodBias; }
NVN_API(nvnSamplerGetCompare, void, (const void* s, int* mode, int* func)) { const SamplerImpl* i = ImplOf<SamplerImpl>(s); *mode = (int)i->desc.compareMode; *func = (int)i->desc.compareFunc; }
NVN_API(nvnSamplerGetBorderColor, void, (const void* s, float* c)) { const SamplerImpl* i = ImplOf<SamplerImpl>(s); for (int k = 0; k < 4; k++) c[k] = i->desc.border[k]; }
NVN_API(nvnSamplerGetBorderColori, void, (const void* s, int32_t* c)) { const SamplerImpl* i = ImplOf<SamplerImpl>(s); for (int k = 0; k < 4; k++) c[k] = (int32_t)i->desc.border[k]; }
NVN_API(nvnSamplerGetBorderColorui, void, (const void* s, uint32_t* c)) { const SamplerImpl* i = ImplOf<SamplerImpl>(s); for (int k = 0; k < 4; k++) c[k] = (uint32_t)i->desc.border[k]; }
NVN_API(nvnSamplerGetMaxAnisotropy, float, (const void* s)) { return ImplOf<SamplerImpl>(s)->desc.maxAniso; }
NVN_API(nvnSamplerGetReductionFilter, int, (const void* s)) { return (int)ImplOf<SamplerImpl>(s)->desc.reduction; }

// ---- texture pool ------------------------------------------------------------------------------------------------------------
struct PoolBacking { void* guestPool; int64_t offset; };    // remembered so the getters answer without a memory pool lookup
static std::unordered_map<const void*, PoolBacking>& Backings() { static std::unordered_map<const void*, PoolBacking> m; return m; }

NVN_API(nvnTexturePoolInitialize, bool, (void* pool, void* memPool, int64_t off, int n)) {
  TexPool* p = new TexPool; p->size = (uint32_t)std::max(0, n); p->ents.resize(p->size);
  SetHdr(pool, kMagicTexPool, p); Backings()[pool] = {memPool, off};
  return true;
}
NVN_API(nvnTexturePoolFinalize, void, (void* pool)) { delete ImplOf<TexPool>(pool); Backings().erase(pool); ClearHdr(pool); }
NVN_API(nvnTexturePoolSetDebugLabel, void, (void* pool, const char* l)) { (void)pool; (void)l; }
NVN_API(nvnTexturePoolGetSize, int, (const void* pool)) { const TexPool* p = ImplOf<TexPool>(pool); return p ? (int)p->size : 0; }
NVN_API(nvnTexturePoolGetMemoryPool, void*, (const void* pool)) { auto it = Backings().find(pool); return it == Backings().end() ? nullptr : it->second.guestPool; }
NVN_API(nvnTexturePoolGetMemoryOffset, int64_t, (const void* pool)) { auto it = Backings().find(pool); return it == Backings().end() ? 0 : it->second.offset; }

static void RegisterTex(void* pool, int id, void* tex, const TextureView* v, bool image) {
  TexPool* p = ImplOf<TexPool>(pool);
  if (!p || id < 0) return;
  if ((uint32_t)id >= p->ents.size()) { LogOnce("texpoolgrow", "texture pool id %d beyond its size %u: growing", id, p->size); p->ents.resize((size_t)id + 1); }
  TexPool::Ent e;
  e.texG = (TextureG*)tex; e.tex = e.texG ? e.texG->impl : nullptr; e.isImage = image;
  if (v) { e.view = *v; e.hasView = v->flags != 0; }
  p->ents[(size_t)id] = e;
}
NVN_API(nvnTexturePoolRegisterTexture, void, (void* pool, int id, void* tex, const TextureView* v)) { RegisterTex(pool, id, tex, v, false); }
NVN_API(nvnTexturePoolRegisterImage, void, (void* pool, int id, void* tex, const TextureView* v)) { RegisterTex(pool, id, tex, v, true); }

// ---- sampler pool --------------------------------------------------------------------------------------------------------------
NVN_API(nvnSamplerPoolInitialize, bool, (void* pool, void* memPool, int64_t off, int n)) {
  SampPool* p = new SampPool; p->size = (uint32_t)std::max(0, n); p->ents.assign(p->size, nullptr); p->owned.assign(p->size, nullptr);
  SetHdr(pool, kMagicSampPool, p); Backings()[pool] = {memPool, off};
  return true;
}
NVN_API(nvnSamplerPoolFinalize, void, (void* pool)) {
  SampPool* p = ImplOf<SampPool>(pool);
  if (p) for (SamplerImpl* s : p->owned) if (s) { SamplerDestroy(s); delete s; }
  delete p; Backings().erase(pool); ClearHdr(pool);
}
NVN_API(nvnSamplerPoolSetDebugLabel, void, (void* pool, const char* l)) { (void)pool; (void)l; }
NVN_API(nvnSamplerPoolGetSize, int, (const void* pool)) { const SampPool* p = ImplOf<SampPool>(pool); return p ? (int)p->size : 0; }
NVN_API(nvnSamplerPoolGetMemoryPool, void*, (const void* pool)) { auto it = Backings().find(pool); return it == Backings().end() ? nullptr : it->second.guestPool; }
NVN_API(nvnSamplerPoolGetMemoryOffset, int64_t, (const void* pool)) { auto it = Backings().find(pool); return it == Backings().end() ? 0 : it->second.offset; }

static void GrowSamp(SampPool* p, int id) {
  if ((uint32_t)id >= p->ents.size()) { LogOnce("samppoolgrow", "sampler pool id %d beyond its size %u: growing", id, p->size); p->ents.resize((size_t)id + 1, nullptr); p->owned.resize((size_t)id + 1, nullptr); }
}
NVN_API(nvnSamplerPoolRegisterSampler, void, (void* pool, int id, const void* samp)) {
  SampPool* p = ImplOf<SampPool>(pool);
  if (!p || id < 0) return;
  GrowSamp(p, id);
  p->ents[(size_t)id] = ImplOf<SamplerImpl>(samp);
}
NVN_API(nvnSamplerPoolRegisterSamplerBuilder, void, (void* pool, int id, const SamplerBuilderG* b)) {
  SampPool* p = ImplOf<SampPool>(pool);
  if (!p || id < 0) return;
  GrowSamp(p, id);
  SamplerImpl* s = new SamplerImpl; s->desc = *b;
  if (!SamplerCreate(s)) { delete s; return; }
  if (SamplerImpl* old = p->owned[(size_t)id]) { SamplerDestroy(old); delete old; }
  p->owned[(size_t)id] = s; p->ents[(size_t)id] = s;
}

void LinkAnchorMem() {}

}  // namespace nvn
