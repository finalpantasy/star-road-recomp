// Star Road Recomp: NVN API, device, queue, sync, window, program (task-12). Our own code (D-008).
#include <algorithm>
#include <cstdlib>

#include "nvn_api.h"

namespace nvn {

// ---- configuration --------------------------------------------------------------------------------------------------
static VkInitOptions g_opt;
static bool g_optSet = false;
void Configure(const VkInitOptions& o) { g_opt = o; g_optSet = true; }
const VkInitOptions& CurrentOptions() {
  if (!g_optSet) {
    g_optSet = true;
    const char* v = getenv("NVN_VALIDATION"); g_opt.validation = v && *v == '1';
    const char* lp = getenv("NVN_LAYER_PATH"); if (lp) g_opt.layerPath = lp;
    const char* pr = getenv("NVN_PRESENT"); g_opt.surfaceExtensions = pr && *pr == '1';
    const char* lg = getenv("NVN_LOG"); if (lg) g_logLevel = atoi(lg);
  }
  return g_opt;
}

static int g_devices = 0;
static uint64_t g_programIds = 0;

// ---- device ------------------------------------------------------------------------------------------------------
NVN_API(nvnDeviceBuilderSetDefaults, void, (DeviceBuilderG* b)) { b->flags = 0; }
NVN_API(nvnDeviceBuilderSetFlags, void, (DeviceBuilderG* b, int flags)) { b->flags = (uint32_t)flags; }

NVN_API(nvnDeviceInitialize, bool, (void* dev, const DeviceBuilderG* b)) {
  if (!VkBringUp(CurrentOptions())) { Err("NVN device initialize failed: %s", LastVkError().c_str()); return false; }
  Device* d = new Device; d->flags = b ? b->flags : 0; d->t0 = (uint64_t)GetTickCount64();
  SetHdr(dev, kMagicDevice, d);
  g_devices++;
  Log("device initialized on %s", g_vk.props.deviceName);
  return true;
}
NVN_API(nvnDeviceFinalize, void, (void* dev)) {
  Device* d = ImplOf<Device>(dev);
  if (!d) return;
  delete d; ClearHdr(dev);
  if (--g_devices == 0) { ReplayShutdown(); VkTearDown(); }
}
NVN_API(nvnDeviceSetDepthMode, void, (void* dev, int mode)) { if (Device* d = ImplOf<Device>(dev)) d->depthMode = (uint32_t)mode; }
NVN_API(nvnDeviceSetWindowOriginMode, void, (void* dev, int mode)) { if (Device* d = ImplOf<Device>(dev)) d->originMode = (uint32_t)mode; }
NVN_API(nvnDeviceRegisterFastClearColor, bool, (void* dev, const float* color, int fmt)) { (void)dev; (void)color; (void)fmt; return true; }   // a hint; Vulkan clears are exact
NVN_API(nvnDeviceRegisterFastClearDepth, bool, (void* dev, float depth)) { (void)dev; (void)depth; return true; }

static int64_t NowNs() {
  LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
  return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
NVN_API(nvnDeviceGetCurrentTimestampInNanoseconds, int64_t, (const void* dev)) { (void)dev; return NowNs(); }
NVN_API(nvnDeviceGetTimestampInNanoseconds, int64_t, (const void* dev, const void* counterData)) {
  (void)dev; (void)counterData;
  LogOnce("ts", "GetTimestampInNanoseconds: counters are not recorded (ReportCounter is a no-op); returning the current time");
  return NowNs();
}

static int InfoValue(int w) {
  const auto& L = g_vk.props.limits;
  switch (w) {
    case NVN_DEVICE_INFO_API_MAJOR_VERSION: return 53;
    case NVN_DEVICE_INFO_API_MINOR_VERSION: return 105;
    case NVN_DEVICE_INFO_UNIFORM_BUFFER_BINDINGS_PER_STAGE: return 14;
    case NVN_DEVICE_INFO_MAX_UNIFORM_BUFFER_SIZE: return 65536;
    case NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT: return 256;
    case NVN_DEVICE_INFO_COLOR_BUFFER_BINDINGS: return 8;
    case NVN_DEVICE_INFO_VERTEX_BUFFER_BINDINGS: return 16;
    case NVN_DEVICE_INFO_TRANSFORM_FEEDBACK_BUFFER_BINDINGS: return 4;
    case NVN_DEVICE_INFO_SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE: return 16;
    case NVN_DEVICE_INFO_TEXTURE_BINDINGS_PER_STAGE: return 32;
    case NVN_DEVICE_INFO_COUNTER_ALIGNMENT: return 16;
    case NVN_DEVICE_INFO_TRANSFORM_FEEDBACK_BUFFER_ALIGNMENT: case NVN_DEVICE_INFO_TRANSFORM_FEEDBACK_CONTROL_ALIGNMENT: return 4;
    case NVN_DEVICE_INFO_INDIRECT_DRAW_ALIGNMENT: return 4;
    case NVN_DEVICE_INFO_VERTEX_ATTRIBUTES: return 16;
    case NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE: case NVN_DEVICE_INFO_SAMPLER_DESCRIPTOR_SIZE: return 32;
    case NVN_DEVICE_INFO_RESERVED_TEXTURE_DESCRIPTORS: case NVN_DEVICE_INFO_RESERVED_SAMPLER_DESCRIPTORS: return 256;
    case NVN_DEVICE_INFO_COMMAND_BUFFER_COMMAND_ALIGNMENT: return 4;
    case NVN_DEVICE_INFO_COMMAND_BUFFER_CONTROL_ALIGNMENT: return 8;
    case NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_COMMAND_SIZE: case NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_CONTROL_SIZE: return 0x1000;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_MINIMUM: return 1;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_RECOMMENDED: return 2;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_ALIGNMENT: return 0x1000;
    case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_GRANULARITY: return 0x20000;
    case NVN_DEVICE_INFO_MAX_TEXTURE_ANISOTROPY: return 16;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_X: case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Y: return 1024;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Z: return 64;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS: return 1024;
    case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X: case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Y:
    case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Z: return 65535;
    case NVN_DEVICE_INFO_IMAGE_BINDINGS_PER_STAGE: return 8;
    case NVN_DEVICE_INFO_MAX_TEXTURE_POOL_SIZE: return 1 << 20;
    case NVN_DEVICE_INFO_MAX_SAMPLER_POOL_SIZE: return 4096;
    case NVN_DEVICE_INFO_MAX_VIEWPORTS: return (int)std::min<uint32_t>(16, L.maxViewports);
    case NVN_DEVICE_INFO_MEMPOOL_TEXTURE_OBJECT_PAGE_ALIGNMENT: return 0x1000;
    case NVN_DEVICE_INFO_SUPPORTS_MIN_MAX_FILTERING: return g_vk.hasMinMax ? 1 : 0;
    case NVN_DEVICE_INFO_SUPPORTS_STENCIL8_FORMAT: return 1;
    case NVN_DEVICE_INFO_SUPPORTS_ASTC_FORMATS: return 1;     // decoded by task-11 when the GPU lacks ASTC
    case NVN_DEVICE_INFO_L2_SIZE: return 0x40000;
    case NVN_DEVICE_INFO_MAX_TEXTURE_LEVELS: return 16;
    case NVN_DEVICE_INFO_MAX_TEXTURE_LAYERS: return 2048;
    // GLSLC code version window: shaders come from our cache, not from glslc output. 0 accepts any version.
    case NVN_DEVICE_INFO_GLSLC_MAX_SUPPORTED_GPU_CODE_MAJOR_VERSION: case NVN_DEVICE_INFO_GLSLC_MIN_SUPPORTED_GPU_CODE_MAJOR_VERSION:
    case NVN_DEVICE_INFO_GLSLC_MAX_SUPPORTED_GPU_CODE_MINOR_VERSION: case NVN_DEVICE_INFO_GLSLC_MIN_SUPPORTED_GPU_CODE_MINOR_VERSION: return 0;
    case NVN_DEVICE_INFO_SUBPIXEL_BITS: return 8;
    case NVN_DEVICE_INFO_INDIRECT_DISPATCH_ALIGNMENT: return 4;
    case NVN_DEVICE_INFO_LINEAR_TEXTURE_STRIDE_ALIGNMENT: case NVN_DEVICE_INFO_LINEAR_RENDER_TARGET_STRIDE_ALIGNMENT: return 32;
    case NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE: return 0x1000;
    case NVN_DEVICE_INFO_UNIFORM_BUFFER_UPDATE_ALIGNMENT: return 4;
    case NVN_DEVICE_INFO_MAX_TEXTURE_SIZE: case NVN_DEVICE_INFO_MAX_CUBE_MAP_TEXTURE_SIZE: case NVN_DEVICE_INFO_MAX_RECTANGLE_TEXTURE_SIZE: return 16384;
    case NVN_DEVICE_INFO_MAX_BUFFER_TEXTURE_SIZE: return 1 << 27;
    case NVN_DEVICE_INFO_MAX_3D_TEXTURE_SIZE: return 2048;
    case NVN_DEVICE_INFO_MAX_PRESENT_INTERVAL: return 4;
    case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_GRANULARITY: case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_GRANULARITY: return 0x1000;
    case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_SIZE: case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_MIN_SIZE: return 0x4000;
    case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_DEFAULT_SIZE: case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_DEFAULT_SIZE: return 0x100000;
    case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_FLUSH_THRESHOLD: return 0x1000;
    // Separate texture and sampler bindings (NVN_DEVICE_FLAG_ENABLE_SEPARATE_SAMPLER_TEXTURE_SUPPORT). Unity sizes its per-program binding
    // tables from these: a 0 leaves them unallocated and the first draw crashes. The counts are what the translated shaders can address:
    // separate textures sit in c[2] at 0x168 + 8 r (r < 128, 0x168..0x568) and separate samplers at 0x568 + 8 s (s < 32).
    case NVN_DEVICE_INFO_SEPARATE_TEXTURE_BINDINGS_PER_STAGE: return 128;
    case NVN_DEVICE_INFO_SEPARATE_SAMPLER_BINDINGS_PER_STAGE: return 32;
    case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_GRANULARITY: return 0x1000;
    case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_MIN_SIZE: return 0x1000;
    case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_DEFAULT_SIZE: return 0x10000;
    case NVN_DEVICE_INFO_DEBUG_GROUPS_MAX_DOMAIN_ID: return 0;
    case NVN_DEVICE_INFO_MAX_TEXTURES_PER_WINDOW: return 4;
    case NVN_DEVICE_INFO_MIN_TEXTURES_PER_WINDOW: return 2;
    case NVN_DEVICE_INFO_MAX_PATCH_SIZE: return 32;
    case NVN_DEVICE_INFO_SHADER_CODE_MEMORY_POOL_PADDING_SIZE: return 0x400;
    default: LogOnce(("devinfo" + std::to_string(w)).c_str(), "DeviceGetInteger: unhandled info 0x%x -> 0", w); return 0;
  }
}
NVN_API(nvnDeviceGetInteger, void, (const void* dev, int what, int* out)) { (void)dev; if (out) *out = InfoValue(what); }

NVN_API(nvnDeviceGetTextureHandle, uint64_t, (const void* dev, int texId, int sampId)) { (void)dev; return ((uint64_t)(uint32_t)sampId << 20) | (uint32_t)texId; }
NVN_API(nvnDeviceGetImageHandle, uint64_t, (const void* dev, int texId)) { (void)dev; return (uint32_t)texId; }
NVN_API(nvnDeviceGetSeparateTextureHandle, uint64_t, (const void* dev, int texId)) { (void)dev; return (uint32_t)texId; }
NVN_API(nvnDeviceGetSeparateSamplerHandle, uint64_t, (const void* dev, int sampId)) { (void)dev; return (uint32_t)sampId; }

NVN_API(nvnDeviceGetProcAddress, void*, (const void* dev, const char* name)) { (void)dev; return ResolveProc(name); }

// ---- queue --------------------------------------------------------------------------------------------------------
NVN_API(nvnQueueBuilderSetDefaults, void, (QueueBuilderG* b)) { void* d = b->dev; memset(b, 0, sizeof *b); b->dev = d; b->cmdMem = 0x100000; b->computeMem = 0x100000; b->controlMem = 0x10000; b->flushThreshold = 0x1000; }
NVN_API(nvnQueueBuilderSetDevice, void, (QueueBuilderG* b, void* dev)) { b->dev = dev; }
NVN_API(nvnQueueBuilderSetCommandMemorySize, void, (QueueBuilderG* b, int64_t n)) { b->cmdMem = (uint64_t)n; }
NVN_API(nvnQueueBuilderSetComputeMemorySize, void, (QueueBuilderG* b, int64_t n)) { b->computeMem = (uint64_t)n; }
NVN_API(nvnQueueBuilderSetControlMemorySize, void, (QueueBuilderG* b, int64_t n)) { b->controlMem = (uint64_t)n; }
NVN_API(nvnQueueBuilderSetCommandFlushThreshold, void, (QueueBuilderG* b, int64_t n)) { b->flushThreshold = (uint64_t)n; }
NVN_API(nvnQueueBuilderGetCommandMemorySize, int64_t, (const QueueBuilderG* b)) { return (int64_t)b->cmdMem; }

NVN_API(nvnQueueInitialize, bool, (void* queue, const QueueBuilderG* b)) {
  QueueImpl* q = new QueueImpl; q->dev = ImplOf<Device>(b->dev);
  if (!QueueCreate(q)) { delete q; return false; }
  SetHdr(queue, kMagicQueue, q);
  return true;
}
NVN_API(nvnQueueFinalize, void, (void* queue)) { QueueImpl* q = ImplOf<QueueImpl>(queue); if (!q) return; QueueDestroy(q); delete q; ClearHdr(queue); }
NVN_API(nvnQueueSetDebugLabel, void, (void* queue, const char* label)) { (void)queue; (void)label; }
NVN_API(nvnQueueSubmitCommands, void, (void* queue, int n, const uint64_t* handles)) { if (QueueImpl* q = ImplOf<QueueImpl>(queue)) QueueSubmit(q, (uint32_t)std::max(0, n), handles); }
NVN_API(nvnQueueFlush, void, (void* queue)) { (void)queue; }      // submits are replayed synchronously into Vulkan
NVN_API(nvnQueueFinish, void, (void* queue)) { if (QueueImpl* q = ImplOf<QueueImpl>(queue)) QueueFinish(q); }
NVN_API(nvnQueueFenceSync, void, (void* queue, void* sync, int cond, int flags)) {
  (void)cond; (void)flags;
  QueueImpl* q = ImplOf<QueueImpl>(queue); Sync* s = ImplOf<Sync>(sync);
  if (q && s) QueueFence(q, s);
}
NVN_API(nvnQueueWaitSync, bool, (void* queue, const void* sync)) {
  (void)queue; const Sync* s = ImplOf<Sync>(sync);
  if (!s || !s->eventId) return true;
  ApiUnlock free;                                  // other threads keep calling NVN while this one waits
  return WaitTimeline(s->value, UINT64_MAX);      // the GPU queue is one in-order queue, so a host wait is equivalent
}
NVN_API(nvnQueuePresentTexture, void, (void* queue, void* window, int index)) {
  QueueImpl* q = ImplOf<QueueImpl>(queue); WindowImpl* w = ImplOf<WindowImpl>(window);
  if (q && w) QueuePresent(q, w, (uint32_t)index);
}

// ---- sync ---------------------------------------------------------------------------------------------------------
NVN_API(nvnSyncInitialize, bool, (void* sync, void* dev)) { (void)dev; SetHdr(sync, kMagicSync, new Sync); return true; }
NVN_API(nvnSyncFinalize, void, (void* sync)) { delete ImplOf<Sync>(sync); ClearHdr(sync); }
NVN_API(nvnSyncSetDebugLabel, void, (void* sync, const char* l)) { (void)sync; (void)l; }
NVN_API(nvnSyncWait, int, (const void* sync, uint64_t timeoutNs)) {
  const Sync* s = ImplOf<Sync>(sync);
  if (!s || !s->eventId || s->value == 0) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
  if (TimelineDone() >= s->value) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
  ApiUnlock free;
  return WaitTimeline(s->value, timeoutNs) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}

// ---- window -------------------------------------------------------------------------------------------------------
NVN_API(nvnWindowBuilderSetDefaults, void, (WindowBuilderG* b)) { void* d = b->dev; memset(b, 0, sizeof *b); b->dev = d; b->numActive = 0; b->presentInterval = 1; }
NVN_API(nvnWindowBuilderSetDevice, void, (WindowBuilderG* b, void* dev)) { b->dev = dev; }
NVN_API(nvnWindowBuilderSetNativeWindow, void, (WindowBuilderG* b, void* native)) { b->native = native; }
NVN_API(nvnWindowBuilderSetNumActiveTextures, void, (WindowBuilderG* b, int n)) { b->numActive = (uint32_t)n; }
NVN_API(nvnWindowBuilderSetPresentInterval, void, (WindowBuilderG* b, int n)) { b->presentInterval = (uint32_t)n; }
NVN_API(nvnWindowBuilderSetTextures, void, (WindowBuilderG* b, int n, void* const* textures)) {
  b->numTextures = (uint32_t)std::min(n, 4);
  for (uint32_t i = 0; i < b->numTextures; i++) b->textures[i] = textures[i];
  if (n > 4) Err("window with %d textures; NVN allows at most 4", n);
}
NVN_API(nvnWindowInitialize, bool, (void* window, const WindowBuilderG* b)) {
  WindowImpl* w = new WindowImpl; w->native = b->native;
  for (uint32_t i = 0; i < b->numTextures; i++) w->textures.push_back((TextureG*)b->textures[i]);
  w->numActive = b->numActive ? b->numActive : b->numTextures; w->presentInterval = b->presentInterval;
  for (auto* t : w->textures) if (t && t->impl) t->impl->presentable = true;
  if (!WindowCreate(w)) { delete w; return false; }
  SetHdr(window, kMagicWindow, w);
  return true;
}
NVN_API(nvnWindowFinalize, void, (void* window)) { WindowImpl* w = ImplOf<WindowImpl>(window); if (!w) return; WindowDestroy(w); delete w; ClearHdr(window); }
NVN_API(nvnWindowSetDebugLabel, void, (void* window, const char* l)) { (void)window; (void)l; }
NVN_API(nvnWindowAcquireTexture, int, (void* window, void* sync, int* index)) {
  WindowImpl* w = ImplOf<WindowImpl>(window);
  if (!w || w->textures.empty()) return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
  uint32_t n = std::max(1u, std::min<uint32_t>(w->numActive, (uint32_t)w->textures.size()));
  if (index) *index = (int)(w->nextIndex % n);
  w->nextIndex++;
  if (Sync* s = ImplOf<Sync>(sync)) { s->value = 0; s->eventId = 1; }       // the image is free immediately
  return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN_API(nvnWindowSetNumActiveTextures, void, (void* window, int n)) { if (WindowImpl* w = ImplOf<WindowImpl>(window)) w->numActive = (uint32_t)n; }
NVN_API(nvnWindowSetPresentInterval, void, (void* window, int n)) { if (WindowImpl* w = ImplOf<WindowImpl>(window)) w->presentInterval = (uint32_t)n; }
NVN_API(nvnWindowGetPresentInterval, int, (const void* window)) { const WindowImpl* w = ImplOf<WindowImpl>(window); return w ? (int)w->presentInterval : 1; }
NVN_API(nvnWindowSetCrop, void, (void* window, int x, int y, int w_, int h)) { if (WindowImpl* w = ImplOf<WindowImpl>(window)) { w->cropX = (uint32_t)x; w->cropY = (uint32_t)y; w->cropW = (uint32_t)w_; w->cropH = (uint32_t)h; } }
NVN_API(nvnWindowGetCrop, void, (const void* window, int32_t* rect)) {
  const WindowImpl* w = ImplOf<WindowImpl>(window);
  if (w) { rect[0] = (int32_t)w->cropX; rect[1] = (int32_t)w->cropY; rect[2] = (int32_t)w->cropW; rect[3] = (int32_t)w->cropH; }
}

void LinkAnchorDevice() {}

// ---- program ------------------------------------------------------------------------------------------------------
NVN_API(nvnProgramInitialize, bool, (void* prog, void* dev)) { (void)dev; ProgramImpl* p = new ProgramImpl; p->id = ++g_programIds; SetHdr(prog, kMagicProgram, p); LiveAdd(p); return true; }
NVN_API(nvnProgramFinalize, void, (void* prog)) { ProgramImpl* p = ImplOf<ProgramImpl>(prog); if (p) { QueueScrub(p); LiveDel(p); delete p; } ClearHdr(prog); }
NVN_API(nvnProgramSetDebugLabel, void, (void* prog, const char* l)) { (void)prog; (void)l; }
NVN_API(nvnProgramSetSampleShading, void, (void* prog, int mode)) { (void)prog; (void)mode; LogOnce("sampleshading", "ProgramSetSampleShading is ignored"); }
NVN_API(nvnProgramSetShaders, bool, (void* prog, int n, const ShaderData* d)) {
  ProgramImpl* p = ImplOf<ProgramImpl>(prog);
  if (!p || n <= 0 || n > 6) return false;
  uint64_t addr[6]; const void* ctl[6];
  for (int i = 0; i < n; i++) { addr[i] = d[i].data; ctl[i] = d[i].control; }
  return ProgramSetShadersImpl(p, (uint32_t)n, addr, ctl);
}

}  // namespace nvn

// The one symbol the guest imports: returns NVN entry points by name. Only the two bootstrap names are served.
extern "C" void* nvn_bootstrap_loader(const char* name) {
  if (!name) return nullptr;
  if (!strcmp(name, "nvnDeviceInitialize") || !strcmp(name, "nvnDeviceGetProcAddress")) return nvn::ResolveProc(name);
  nvn::LogOnce(name, "nvnBootstrapLoader: unknown name '%s'", name);
  return nullptr;
}
