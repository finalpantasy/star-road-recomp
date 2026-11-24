// Star Road Recomp: NVN API, state objects and command buffers (task-12). Our own code (D-008).
//
// Command buffers record into a CmdList (nvn_internal.h, conventions listed next to Op); the list is replayed into Vulkan when the
// queue submits its handle (nvn_replay.cpp).
#include <algorithm>

#include "nvn_api.h"

namespace nvn {

// ---- state objects (plain data stored in the game's blobs) ---------------------------------------------------------------
NVN_API(nvnBlendStateSetDefaults, void, (BlendStateG* s)) {
  *s = BlendStateG{};
  s->srcRGB = s->srcA = NVN_BLEND_FUNC_ONE; s->dstRGB = s->dstA = NVN_BLEND_FUNC_ZERO;
  s->eq = (uint8_t)(NVN_BLEND_EQUATION_ADD | (NVN_BLEND_EQUATION_ADD << 3));
}
NVN_API(nvnBlendStateSetBlendTarget, void, (BlendStateG* s, int target)) { s->target = (uint8_t)target; }
NVN_API(nvnBlendStateSetBlendFunc, void, (BlendStateG* s, int srcRGB, int dstRGB, int srcA, int dstA)) {
  s->srcRGB = (uint8_t)srcRGB; s->dstRGB = (uint8_t)dstRGB; s->srcA = (uint8_t)srcA; s->dstA = (uint8_t)dstA;
}
NVN_API(nvnBlendStateSetBlendEquation, void, (BlendStateG* s, int rgb, int a)) { s->eq = (uint8_t)((s->eq & 0xC0) | (rgb & 7) | ((a & 7) << 3)); }
NVN_API(nvnBlendStateSetAdvancedMode, void, (BlendStateG* s, int mode)) { s->advMode = (uint8_t)mode; if (mode) LogOnce("advblend", "advanced blend mode %d is not supported (plain blending used)", mode); }
NVN_API(nvnBlendStateSetAdvancedOverlap, void, (BlendStateG* s, int o)) { s->eq = (uint8_t)((s->eq & 0x3F) | ((o & 3) << 6)); }
NVN_API(nvnBlendStateGetAdvancedNormalizedDst, bool, (const BlendStateG* s)) { (void)s; return false; }

NVN_API(nvnColorStateSetDefaults, void, (ColorStateG* s)) { s->blendMask = 0; s->logicOp = NVN_LOGIC_OP_COPY; s->alphaFunc = NVN_ALPHA_FUNC_ALWAYS; s->pad = 0; }
NVN_API(nvnColorStateSetBlendEnable, void, (ColorStateG* s, int target, bool on)) { if (target >= 0 && target < 8) s->blendMask = (uint8_t)(on ? s->blendMask | (1u << target) : s->blendMask & ~(1u << target)); }
NVN_API(nvnColorStateGetBlendEnable, bool, (const ColorStateG* s, int target)) { return target >= 0 && target < 8 && ((s->blendMask >> target) & 1); }
NVN_API(nvnColorStateSetLogicOp, void, (ColorStateG* s, int op)) { s->logicOp = (uint8_t)op; }
NVN_API(nvnColorStateSetAlphaTest, void, (ColorStateG* s, int f)) { s->alphaFunc = (uint8_t)f; }

NVN_API(nvnChannelMaskStateSetDefaults, void, (ChannelMaskG* s)) { s->masks = 0xFFFFFFFFu; }
NVN_API(nvnChannelMaskStateSetChannelMask, void, (ChannelMaskG* s, int target, bool r, bool g, bool b, bool a)) {
  if (target < 0 || target > 7) return;
  uint32_t m = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
  s->masks = (s->masks & ~(0xFu << (4 * target))) | (m << (4 * target));
}

NVN_API(nvnDepthStencilStateSetDefaults, void, (DepthStencilG* s)) {
  *s = DepthStencilG{};
  s->flags = 3;                                              // depth test and write on, stencil off
  s->depthFunc = NVN_DEPTH_FUNC_LESS;
  s->func = (uint8_t)(NVN_STENCIL_FUNC_ALWAYS | (NVN_STENCIL_FUNC_ALWAYS << 4));
  s->opF = s->opB = (uint8_t)(NVN_STENCIL_OP_KEEP | (NVN_STENCIL_OP_KEEP << 4)); s->opF2 = s->opB2 = NVN_STENCIL_OP_KEEP;
}
static void Bit(uint8_t* f, uint8_t bit, bool on) { *f = (uint8_t)(on ? *f | bit : *f & ~bit); }
NVN_API(nvnDepthStencilStateSetDepthTestEnable, void, (DepthStencilG* s, bool on)) { Bit(&s->flags, 1, on); }
NVN_API(nvnDepthStencilStateSetDepthWriteEnable, void, (DepthStencilG* s, bool on)) { Bit(&s->flags, 2, on); }
NVN_API(nvnDepthStencilStateSetDepthFunc, void, (DepthStencilG* s, int f)) { s->depthFunc = (uint8_t)f; }
NVN_API(nvnDepthStencilStateSetStencilTestEnable, void, (DepthStencilG* s, bool on)) { Bit(&s->flags, 4, on); }
NVN_API(nvnDepthStencilStateSetStencilFunc, void, (DepthStencilG* s, int face, int f)) {
  if (face & NVN_FACE_FRONT) s->func = (uint8_t)((s->func & 0xF0) | (f & 15));
  if (face & NVN_FACE_BACK) s->func = (uint8_t)((s->func & 0x0F) | ((f & 15) << 4));
}
NVN_API(nvnDepthStencilStateSetStencilOp, void, (DepthStencilG* s, int face, int fail, int zfail, int zpass)) {
  if (face & NVN_FACE_FRONT) { s->opF = (uint8_t)((fail & 15) | ((zfail & 15) << 4)); s->opF2 = (uint8_t)zpass; }
  if (face & NVN_FACE_BACK) { s->opB = (uint8_t)((fail & 15) | ((zfail & 15) << 4)); s->opB2 = (uint8_t)zpass; }
}

NVN_API(nvnMultisampleStateSetDefaults, void, (MultisampleG* s)) { memset(s, 0, sizeof *s); }
NVN_API(nvnMultisampleStateSetSamples, void, (MultisampleG* s, int n)) { s->samples = (uint8_t)n; s->rasterSamples = (uint8_t)n; }
NVN_API(nvnMultisampleStateSetMultisampleEnable, void, (MultisampleG* s, bool on)) { s->enable = on; }
NVN_API(nvnMultisampleStateSetAlphaToCoverageEnable, void, (MultisampleG* s, bool on)) { s->a2c = on; }
NVN_API(nvnMultisampleStateGetSamples, int, (const MultisampleG* s)) { return s->samples; }
NVN_API(nvnMultisampleStateGetRasterSamples, int, (const MultisampleG* s)) { return s->rasterSamples; }
NVN_API(nvnMultisampleStateGetMultisampleEnable, bool, (const MultisampleG* s)) { return s->enable != 0; }
NVN_API(nvnMultisampleStateGetSampleLocationsGridEnable, bool, (const MultisampleG* s)) { return s->grid != 0; }

NVN_API(nvnPolygonStateSetDefaults, void, (PolygonStateG* s)) { s->cull = NVN_FACE_BACK; s->front = NVN_FRONT_FACE_CCW; s->mode = NVN_POLYGON_MODE_FILL; s->offsets = 0; }
NVN_API(nvnPolygonStateSetCullFace, void, (PolygonStateG* s, int f)) { s->cull = (uint8_t)f; }
NVN_API(nvnPolygonStateSetFrontFace, void, (PolygonStateG* s, int f)) { s->front = (uint8_t)f; }
NVN_API(nvnPolygonStateSetPolygonMode, void, (PolygonStateG* s, int m)) { s->mode = (uint8_t)m; }
NVN_API(nvnPolygonStateSetPolygonOffsetEnables, void, (PolygonStateG* s, int bits)) { s->offsets = (uint8_t)bits; }

NVN_API(nvnVertexAttribStateSetDefaults, void, (VertexAttribG* s)) { *s = VertexAttribG{}; }
NVN_API(nvnVertexAttribStateSetFormat, void, (VertexAttribG* s, int fmt, int64_t rel)) { s->format = (uint8_t)fmt; s->offset = (uint16_t)rel; }
NVN_API(nvnVertexAttribStateSetStreamIndex, void, (VertexAttribG* s, int idx)) { s->stream = (uint8_t)idx; }
NVN_API(nvnVertexStreamStateSetDefaults, void, (VertexStreamG* s)) { *s = VertexStreamG{0, 0}; }
NVN_API(nvnVertexStreamStateSetStride, void, (VertexStreamG* s, int64_t stride)) { s->stride = (uint32_t)stride; }

// ---- command buffer ------------------------------------------------------------------------------------------------------
static constexpr size_t kKeepLists = 32;    // handles a command buffer keeps alive after EndRecording

static CbImpl* CB(void* cb) { return ImplOf<CbImpl>(cb); }
static Cmd* Emit(void* cb, Op op) {
  CbImpl* c = CB(cb);
  if (!c || !c->recording || !c->cur) { LogOnce("notrec", "command recorded outside BeginRecording/EndRecording (dropped)"); return nullptr; }
  c->cur->cmds.emplace_back(); Cmd* m = &c->cur->cmds.back(); m->op = op; return m;
}
#define EMIT(OP) Cmd* m = Emit(cb, Op::OP); if (!m) return

NVN_API(nvnCommandBufferInitialize, bool, (void* cb, void* dev)) {
  CbImpl* c = new CbImpl; c->dev = ImplOf<Device>(dev);
  SetHdr(cb, kMagicCmd, c);
  return true;
}
NVN_API(nvnCommandBufferFinalize, void, (void* cb)) {
  CbImpl* c = CB(cb); if (!c) return;
  for (uint64_t id : c->produced) ReleaseList(id);
  delete c; ClearHdr(cb);
}
NVN_API(nvnCommandBufferBeginRecording, void, (void* cb)) {
  CbImpl* c = CB(cb); if (!c) return;
  c->cur = std::make_shared<CmdList>(); c->recording = true;
}
NVN_API(nvnCommandBufferEndRecording, uint64_t, (void* cb)) {
  CbImpl* c = CB(cb); if (!c || !c->cur) return 0;
  c->recording = false;
  uint64_t id = RegisterList(c->cur);
  c->cur->id = id; c->cur.reset();
  c->produced.push_back(id);
  while (c->produced.size() > kKeepLists) { ReleaseList(c->produced.front()); c->produced.erase(c->produced.begin()); }
  return id;
}
NVN_API(nvnCommandBufferIsRecording, bool, (const void* cb)) { const CbImpl* c = CB((void*)cb); return c && c->recording; }
NVN_API(nvnCommandBufferAddCommandMemory, void, (void* cb, void* pool, int64_t off, size_t size)) { (void)cb; (void)pool; (void)off; (void)size; }   // lists live on the host heap
NVN_API(nvnCommandBufferAddControlMemory, void, (void* cb, void* mem, size_t size)) { (void)cb; (void)mem; (void)size; }
NVN_API(nvnCommandBufferSetMemoryCallback, void, (void* cb, void* fn)) { if (CbImpl* c = CB(cb)) c->memCb = fn; }       // never called: memory never runs out
NVN_API(nvnCommandBufferSetMemoryCallbackData, void, (void* cb, void* data)) { if (CbImpl* c = CB(cb)) c->memCbData = data; }
NVN_API(nvnCommandBufferPopDebugGroup, void, (void* cb)) { (void)cb; }
NVN_API(nvnCommandBufferSaveZCullData, void, (void* cb, uint64_t a, size_t s)) { (void)cb; (void)a; (void)s; }
NVN_API(nvnCommandBufferRestoreZCullData, void, (void* cb, uint64_t a, size_t s)) { (void)cb; (void)a; (void)s; }
NVN_API(nvnCommandBufferSetColorReductionEnable, void, (void* cb, bool on)) { (void)cb; (void)on; }
NVN_API(nvnCommandBufferSetConservativeRasterEnable, void, (void* cb, bool on)) { (void)cb; if (on) LogOnce("conserv", "conservative rasterization is not supported"); }
NVN_API(nvnCommandBufferSetConservativeRasterDilate, void, (void* cb, float v)) { (void)cb; (void)v; }
NVN_API(nvnCommandBufferSetShaderScratchMemory, void, (void* cb, void* pool, int64_t off, size_t size)) { (void)cb; (void)pool; (void)off; (void)size; }
NVN_API(nvnCommandBufferSetSubpixelPrecisionBias, void, (void* cb, int x, int y)) { (void)cb; (void)x; (void)y; }
NVN_API(nvnCommandBufferSetTiledCacheAction, void, (void* cb, int a)) { (void)cb; (void)a; }

// state binds: the state struct is copied into the command
NVN_API(nvnCommandBufferBindBlendState, void, (void* cb, const BlendStateG* s)) { EMIT(BindBlend); memcpy(m->st, s, sizeof(BlendStateG)); }
NVN_API(nvnCommandBufferBindColorState, void, (void* cb, const ColorStateG* s)) { EMIT(BindColor); memcpy(m->st, s, sizeof(ColorStateG)); }
NVN_API(nvnCommandBufferBindChannelMaskState, void, (void* cb, const ChannelMaskG* s)) { EMIT(BindChannelMask); memcpy(m->st, s, sizeof(ChannelMaskG)); }
NVN_API(nvnCommandBufferBindDepthStencilState, void, (void* cb, const DepthStencilG* s)) { EMIT(BindDepthStencil); memcpy(m->st, s, sizeof(DepthStencilG)); }
NVN_API(nvnCommandBufferBindMultisampleState, void, (void* cb, const MultisampleG* s)) { EMIT(BindMultisample); memcpy(m->st, s, sizeof(MultisampleG)); }
NVN_API(nvnCommandBufferBindPolygonState, void, (void* cb, const PolygonStateG* s)) { EMIT(BindPolygon); memcpy(m->st, s, sizeof(PolygonStateG)); }
NVN_API(nvnCommandBufferBindVertexAttribState, void, (void* cb, int n, const VertexAttribG* a)) {
  { Cmd* r = Emit(cb, Op::BindVertexAttrib); if (!r) return; r->i[0] = -1; r->i[1] = 1; }          // a new array replaces the old one
  for (int i = 0; i < n; i++) { Cmd* m = Emit(cb, Op::BindVertexAttrib); if (!m) return; m->i[0] = i; m->i[1] = 0; memcpy(m->st, &a[i], sizeof(VertexAttribG)); }
}
NVN_API(nvnCommandBufferBindVertexStreamState, void, (void* cb, int n, const VertexStreamG* s)) {
  for (int i = 0; i < n; i++) { Cmd* m = Emit(cb, Op::BindVertexStream); if (!m) return; m->i[0] = i; memcpy(m->st, &s[i], sizeof(VertexStreamG)); }
}
NVN_API(nvnCommandBufferBindProgram, void, (void* cb, const void* prog, int stages)) { EMIT(BindProgram); m->p[0] = (uint64_t)(uintptr_t)ImplOf<ProgramImpl>(prog); m->i[0] = stages; }

// buffer and resource binds
NVN_API(nvnCommandBufferBindVertexBuffer, void, (void* cb, int idx, uint64_t addr, int64_t size)) { EMIT(BindVertexBuffer); m->i[0] = idx; m->l[0] = (int64_t)addr; m->l[1] = size; }
NVN_API(nvnCommandBufferBindVertexBuffers, void, (void* cb, int first, int n, const BufferRange* r)) {
  EMIT(BindVertexBuffers); m->i[0] = first; m->i[1] = n; m->blob.assign((const uint8_t*)r, (const uint8_t*)r + 16 * (size_t)std::max(0, n));
}
NVN_API(nvnCommandBufferBindUniformBuffer, void, (void* cb, int stage, int idx, uint64_t addr, int64_t size)) { EMIT(BindUniform); m->i[0] = stage; m->i[1] = idx; m->l[0] = (int64_t)addr; m->l[1] = size; }
NVN_API(nvnCommandBufferBindUniformBuffers, void, (void* cb, int stage, int first, int n, const BufferRange* r)) {
  EMIT(BindUniforms); m->i[0] = stage; m->i[1] = first; m->i[2] = n; m->blob.assign((const uint8_t*)r, (const uint8_t*)r + 16 * (size_t)std::max(0, n));
}
NVN_API(nvnCommandBufferBindStorageBuffer, void, (void* cb, int stage, int idx, uint64_t addr, int64_t size)) { EMIT(BindStorage); m->i[0] = stage; m->i[1] = idx; m->l[0] = (int64_t)addr; m->l[1] = size; }
NVN_API(nvnCommandBufferBindSeparateTexture, void, (void* cb, int stage, int idx, uint64_t h)) { EMIT(BindSepTexture); m->i[0] = stage; m->i[1] = idx; m->i[2] = 0x168; m->l[0] = (int64_t)h; }
NVN_API(nvnCommandBufferBindSeparateSampler, void, (void* cb, int stage, int idx, uint64_t h)) { EMIT(BindSepSampler); m->i[0] = stage; m->i[1] = idx; m->l[0] = (int64_t)h; }
NVN_API(nvnCommandBufferBindSeparateSamplers, void, (void* cb, int stage, int first, int n, const uint64_t* h)) {
  for (int i = 0; i < n; i++) { Cmd* m = Emit(cb, Op::BindSepSampler); if (!m) return; m->i[0] = stage; m->i[1] = first + i; m->l[0] = (int64_t)h[i]; }
}
NVN_API(nvnCommandBufferBindImage, void, (void* cb, int stage, int idx, uint64_t h)) { EMIT(BindImage); m->i[0] = stage; m->i[1] = idx; m->l[0] = (int64_t)h; }
NVN_API(nvnCommandBufferSetTexturePool, void, (void* cb, const void* pool)) { EMIT(SetTexturePool); m->p[0] = (uint64_t)(uintptr_t)ImplOf<TexPool>(pool); }
NVN_API(nvnCommandBufferSetSamplerPool, void, (void* cb, const void* pool)) { EMIT(SetSamplerPool); m->p[0] = (uint64_t)(uintptr_t)ImplOf<SampPool>(pool); }

// render state
NVN_API(nvnCommandBufferSetViewport, void, (void* cb, int x, int y, int w, int h)) { EMIT(SetViewport); m->i[0] = x; m->i[1] = y; m->i[2] = w; m->i[3] = h; }
NVN_API(nvnCommandBufferSetScissor, void, (void* cb, int x, int y, int w, int h)) { EMIT(SetScissor); m->i[0] = x; m->i[1] = y; m->i[2] = w; m->i[3] = h; }
NVN_API(nvnCommandBufferSetDepthRange, void, (void* cb, float n, float f)) { EMIT(SetDepthRange); m->f[0] = n; m->f[1] = f; }
NVN_API(nvnCommandBufferSetDepthBounds, void, (void* cb, bool on, float lo, float hi)) { EMIT(SetDepthBounds); m->i[0] = on; m->f[0] = lo; m->f[1] = hi; }
NVN_API(nvnCommandBufferSetDepthClamp, void, (void* cb, bool on)) { EMIT(SetDepthClamp); m->i[0] = on; }
NVN_API(nvnCommandBufferSetLineWidth, void, (void* cb, float w)) { EMIT(SetLineWidth); m->f[0] = w; }
NVN_API(nvnCommandBufferSetPointSize, void, (void* cb, float s)) { EMIT(SetPointSize); m->f[0] = s; }
NVN_API(nvnCommandBufferSetPatchSize, void, (void* cb, int n)) { EMIT(SetPatchSize); m->i[0] = n; }
NVN_API(nvnCommandBufferSetPolygonOffsetClamp, void, (void* cb, float factor, float units, float clamp)) { EMIT(SetPolyOffset); m->f[0] = factor; m->f[1] = units; m->f[2] = clamp; }
NVN_API(nvnCommandBufferSetPrimitiveRestart, void, (void* cb, bool on, int idx)) { EMIT(SetPrimRestart); m->i[0] = on; m->i[1] = idx; }
NVN_API(nvnCommandBufferSetRasterizerDiscard, void, (void* cb, bool on)) { EMIT(SetRasterDiscard); m->i[0] = on; }
NVN_API(nvnCommandBufferSetSampleMask, void, (void* cb, int mask)) { EMIT(SetSampleMask); m->i[0] = mask; }
NVN_API(nvnCommandBufferSetStencilRef, void, (void* cb, int face, int v)) { EMIT(SetStencilRef); m->i[0] = face; m->i[1] = v; }
NVN_API(nvnCommandBufferSetStencilMask, void, (void* cb, int face, int v)) { EMIT(SetStencilMask); m->i[0] = face; m->i[1] = v; }
NVN_API(nvnCommandBufferSetStencilValueMask, void, (void* cb, int face, int v)) { EMIT(SetStencilValueMask); m->i[0] = face; m->i[1] = v; }
NVN_API(nvnCommandBufferSetRenderTargets, void, (void* cb, int n, TextureG* const* tex, const TextureView* const* views, TextureG* depth, const TextureView* depthView)) {
  EMIT(SetRenderTargets);
  m->i[0] = n; m->blob.assign(216, 0);
  for (int i = 0; i < std::min(n, 8); i++) {
    memcpy(m->blob.data() + 8 * i, &tex[i], 8);
    if (views && views[i]) memcpy(m->blob.data() + 64 + 16 * i, views[i], 16);
  }
  memcpy(m->blob.data() + 192, &depth, 8);
  if (depthView) memcpy(m->blob.data() + 200, depthView, 16);
}

// draws and dispatches
NVN_API(nvnCommandBufferDrawArrays, void, (void* cb, int prim, int first, int count)) { EMIT(Draw); m->i[0] = prim; m->i[1] = first; m->i[2] = count; }
NVN_API(nvnCommandBufferDrawArraysInstanced, void, (void* cb, int prim, int first, int count, int baseInst, int nInst)) {
  EMIT(DrawInstanced); m->i[0] = prim; m->i[1] = first; m->i[2] = count; m->i[3] = baseInst; m->i[4] = nInst;
}
NVN_API(nvnCommandBufferDrawArraysIndirect, void, (void* cb, int prim, uint64_t indirect)) { EMIT(DrawIndirect); m->i[0] = prim; m->l[1] = (int64_t)indirect; }
NVN_API(nvnCommandBufferDrawElementsBaseVertex, void, (void* cb, int prim, int type, int count, uint64_t idx, int baseVertex)) {
  EMIT(DrawElements); m->i[0] = prim; m->i[1] = type; m->i[2] = count; m->l[0] = (int64_t)idx; m->i[3] = baseVertex;
}
NVN_API(nvnCommandBufferDrawElementsInstanced, void, (void* cb, int prim, int type, int count, uint64_t idx, int baseVertex, int baseInst, int nInst)) {
  EMIT(DrawElementsInstanced); m->i[0] = prim; m->i[1] = type; m->i[2] = count; m->l[0] = (int64_t)idx; m->i[3] = baseVertex; m->i[4] = baseInst; m->i[5] = nInst;
}
NVN_API(nvnCommandBufferDrawElementsIndirect, void, (void* cb, int prim, int type, uint64_t idx, uint64_t indirect)) {
  EMIT(DrawElementsIndirect); m->i[0] = prim; m->i[1] = type; m->l[0] = (int64_t)idx; m->l[1] = (int64_t)indirect;
}
NVN_API(nvnCommandBufferDispatchCompute, void, (void* cb, int x, int y, int z)) { EMIT(Dispatch); m->i[0] = x; m->i[1] = y; m->i[2] = z; }
NVN_API(nvnCommandBufferDispatchComputeIndirect, void, (void* cb, uint64_t indirect)) { EMIT(DispatchIndirect); m->l[0] = (int64_t)indirect; }

// clears, copies, sync
NVN_API(nvnCommandBufferClearColor, void, (void* cb, int idx, const float* c, int mask)) { EMIT(ClearColor); m->i[0] = idx; m->i[1] = mask; memcpy(m->st, c, 16); }
NVN_API(nvnCommandBufferClearDepthStencil, void, (void* cb, float depth, bool depthMask, int stencil, int stencilMask)) {
  EMIT(ClearDepthStencil); m->f[0] = depth; m->i[0] = depthMask; m->i[1] = stencil; m->i[2] = stencilMask != 0;
}
NVN_API(nvnCommandBufferCopyBufferToBuffer, void, (void* cb, uint64_t src, uint64_t dst, int64_t size, int flags)) {
  (void)flags; EMIT(CopyBufToBuf); m->l[0] = (int64_t)src; m->l[1] = (int64_t)dst; m->l[2] = size;
}
static void PackVR(uint8_t* out, const TextureView* v, const CopyRegion* r) {
  if (v) memcpy(out, v, 16); else memset(out, 0, 16);
  if (r) memcpy(out + 16, r, 24); else memset(out + 16, 0, 24);
}
NVN_API(nvnCommandBufferCopyBufferToTexture, void, (void* cb, uint64_t src, TextureG* dst, const TextureView* v, const CopyRegion* r, int flags)) {
  (void)flags; EMIT(CopyBufToTex); m->obj[0] = dst; m->l[0] = (int64_t)src; m->blob.resize(40); PackVR(m->blob.data(), v, r);
}
NVN_API(nvnCommandBufferCopyTextureToBuffer, void, (void* cb, TextureG* src, const TextureView* v, const CopyRegion* r, uint64_t dst, int flags)) {
  (void)flags; EMIT(CopyTexToBuf); m->obj[0] = src; m->l[0] = (int64_t)dst; m->blob.resize(40); PackVR(m->blob.data(), v, r);
}
NVN_API(nvnCommandBufferCopyTextureToTexture, void, (void* cb, TextureG* src, const TextureView* sv, const CopyRegion* sr, TextureG* dst, const TextureView* dv, const CopyRegion* dr, int flags)) {
  EMIT(CopyTexToTex); m->obj[0] = src; m->obj[1] = dst; m->i[0] = flags; m->blob.resize(80);
  PackVR(m->blob.data(), sv, sr); PackVR(m->blob.data() + 40, dv, dr);
}
NVN_API(nvnCommandBufferDownsample, void, (void* cb, TextureG* src, TextureG* dst)) { EMIT(Downsample); m->obj[0] = src; m->obj[1] = dst; }
NVN_API(nvnCommandBufferBarrier, void, (void* cb, int bits)) { EMIT(Barrier); m->i[0] = bits; }
NVN_API(nvnCommandBufferFenceSync, void, (void* cb, void* sync, int cond, int flags)) { (void)cond; (void)flags; EMIT(FenceSync); m->p[0] = (uint64_t)(uintptr_t)ImplOf<Sync>(sync); }
NVN_API(nvnCommandBufferReportCounter, void, (void* cb, int counter, uint64_t addr)) {
  (void)counter; (void)addr; EMIT(ReportCounter);
  LogOnce("counter", "ReportCounter is a no-op: nothing is written to the counter address");
}
NVN_API(nvnCommandBufferUpdateUniformBuffer, void, (void* cb, uint64_t addr, int64_t bufSize, int64_t off, int64_t size, const void* data)) {
  EMIT(UpdateUniform); m->l[0] = (int64_t)addr; m->l[1] = bufSize; m->l[2] = off; m->l[3] = size;
  m->blob.assign((const uint8_t*)data, (const uint8_t*)data + (size_t)std::max<int64_t>(0, size));
}
NVN_API(nvnCommandBufferCallCommands, void, (void* cb, int n, const uint64_t* handles)) {
  EMIT(CallCommands); m->i[0] = n; m->blob.assign((const uint8_t*)handles, (const uint8_t*)handles + 8 * (size_t)std::max(0, n));
}

void LinkAnchorCmd() {}

}  // namespace nvn
