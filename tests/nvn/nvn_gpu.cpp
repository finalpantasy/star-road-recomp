// Star Road Recomp: NVN on Vulkan GPU test (task-12). Our own code (D-008).
//
// Runs the real NVN procs (resolved by name, like the game does) on the machine's Vulkan device with the validation layer on:
//   1. clear a render target and read it back
//   2. textured quad (separate texture and sampler handles, uniform buffer tint), texture bytes block-linear swizzled with task-11
//   3. RGBA4 component order (NVN names components from the least significant bit)
//   4. depth range: NVN_DEPTH_MODE_NEAR_IS_ZERO clips z outside 0..1 and stores z as depth; NEAR_IS_MINUS_W maps -1..1 to 0..1
//   5. sync objects, buffer copies, uniform buffer update, texture to buffer copy
//   6. a window presented through a hidden surface (never shown) when the device can present
//   7. the NVNtexture blob keeps to its 0x80 bytes
// and asserts the validation layer reported nothing.
//
// usage: nvn_gpu [--no-present] [--no-validation] [--out DIR]       exit 0 pass, 1 fail, 3 not run (no usable Vulkan device)
// env:   NVN_LAYER_PATH (validation layer folder), NVN_GLSLC (glslc.exe), NVN_TEST_DIR (this folder), NVN_OUT_DIR
#include <malloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../runtime/gfx/texture/texture.h"
#include "nvn_api.h"

using namespace nvn;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                                                                       \
  do {                                                                                                         \
    g_checks++;                                                                                                \
    if (!(cond)) { g_fails++; printf("FAIL line %d  %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

// ---- the NVN procs this test calls (looked up by name at start) ----------------------------------------------------------
#define PROCS(X)                                                                                                         \
  X(void, nvnDeviceBuilderSetDefaults, (void*))                                                                          \
  X(void, nvnDeviceBuilderSetFlags, (void*, int))                                                                        \
  X(bool, nvnDeviceInitialize, (void*, const void*))                                                                     \
  X(void, nvnDeviceFinalize, (void*))                                                                                    \
  X(void, nvnDeviceSetDepthMode, (void*, int))                                                                           \
  X(void, nvnDeviceSetWindowOriginMode, (void*, int))                                                                    \
  X(void, nvnDeviceGetInteger, (const void*, int, int*))                                                                 \
  X(uint64_t, nvnDeviceGetSeparateTextureHandle, (const void*, int))                                                     \
  X(uint64_t, nvnDeviceGetSeparateSamplerHandle, (const void*, int))                                                     \
  X(void, nvnQueueBuilderSetDefaults, (void*))                                                                           \
  X(void, nvnQueueBuilderSetDevice, (void*, void*))                                                                      \
  X(bool, nvnQueueInitialize, (void*, const void*))                                                                      \
  X(void, nvnQueueFinalize, (void*))                                                                                     \
  X(void, nvnQueueSubmitCommands, (void*, int, const uint64_t*))                                                         \
  X(void, nvnQueueFlush, (void*))                                                                                        \
  X(void, nvnQueueFinish, (void*))                                                                                       \
  X(void, nvnQueueFenceSync, (void*, void*, int, int))                                                                   \
  X(bool, nvnQueueWaitSync, (void*, const void*))                                                                        \
  X(void, nvnQueuePresentTexture, (void*, void*, int))                                                                   \
  X(bool, nvnSyncInitialize, (void*, void*))                                                                             \
  X(void, nvnSyncFinalize, (void*))                                                                                      \
  X(int, nvnSyncWait, (const void*, uint64_t))                                                                           \
  X(void, nvnMemoryPoolBuilderSetDefaults, (void*))                                                                      \
  X(void, nvnMemoryPoolBuilderSetDevice, (void*, void*))                                                                 \
  X(void, nvnMemoryPoolBuilderSetFlags, (void*, int))                                                                    \
  X(void, nvnMemoryPoolBuilderSetStorage, (void*, void*, size_t))                                                        \
  X(bool, nvnMemoryPoolInitialize, (void*, const void*))                                                                 \
  X(void, nvnMemoryPoolFinalize, (void*))                                                                                \
  X(uint64_t, nvnMemoryPoolGetBufferAddress, (const void*))                                                              \
  X(void*, nvnMemoryPoolMap, (const void*))                                                                              \
  X(void, nvnMemoryPoolFlushMappedRange, (const void*, int64_t, size_t))                                                 \
  X(void, nvnBufferBuilderSetDefaults, (void*))                                                                          \
  X(void, nvnBufferBuilderSetDevice, (void*, void*))                                                                     \
  X(void, nvnBufferBuilderSetStorage, (void*, void*, int64_t, int64_t))                                                  \
  X(bool, nvnBufferInitialize, (void*, const void*))                                                                     \
  X(void, nvnBufferFinalize, (void*))                                                                                    \
  X(void*, nvnBufferMap, (const void*))                                                                                  \
  X(uint64_t, nvnBufferGetAddress, (const void*))                                                                        \
  X(void, nvnBufferFlushMappedRange, (const void*, int64_t, size_t))                                                     \
  X(void, nvnTextureBuilderSetDefaults, (void*))                                                                         \
  X(void, nvnTextureBuilderSetDevice, (void*, void*))                                                                    \
  X(void, nvnTextureBuilderSetTarget, (void*, int))                                                                      \
  X(void, nvnTextureBuilderSetSize3D, (void*, int, int, int))                                                           \
  X(void, nvnTextureBuilderSetSamples, (void*, int))                                                                    \
  X(void, nvnTextureBuilderSetLevels, (void*, int))                                                                     \
  X(void, nvnTextureBuilderSetSize2D, (void*, int, int))                                                                 \
  X(void, nvnTextureBuilderSetFormat, (void*, int))                                                                      \
  X(void, nvnTextureBuilderSetFlags, (void*, int))                                                                       \
  X(void, nvnTextureBuilderSetStorage, (void*, void*, int64_t))                                                          \
  X(int64_t, nvnTextureBuilderGetStorageSize, (const void*))                                                             \
  X(int64_t, nvnTextureBuilderGetStorageAlignment, (const void*))                                                        \
  X(bool, nvnTextureInitialize, (void*, const void*))                                                                    \
  X(void, nvnTextureFinalize, (void*))                                                                                   \
  X(void, nvnTextureWriteTexels, (void*, const void*, const void*, const void*))                                         \
  X(int64_t, nvnTextureGetStorageSize, (const void*))                                                                    \
  X(int, nvnTextureGetWidth, (const void*))                                                                              \
  X(int, nvnTextureGetFormat, (const void*))                                                                             \
  X(void, nvnSamplerBuilderSetDefaults, (void*))                                                                         \
  X(void, nvnSamplerBuilderSetDevice, (void*, void*))                                                                    \
  X(void, nvnSamplerBuilderSetMinMagFilter, (void*, int, int))                                                           \
  X(void, nvnSamplerBuilderSetWrapMode, (void*, int, int, int))                                                          \
  X(bool, nvnSamplerInitialize, (void*, const void*))                                                                    \
  X(void, nvnSamplerFinalize, (void*))                                                                                   \
  X(bool, nvnTexturePoolInitialize, (void*, void*, int64_t, int))                                                        \
  X(void, nvnTexturePoolRegisterTexture, (void*, int, void*, const void*))                                               \
  X(void, nvnTexturePoolFinalize, (void*))                                                                               \
  X(bool, nvnSamplerPoolInitialize, (void*, void*, int64_t, int))                                                        \
  X(void, nvnSamplerPoolRegisterSampler, (void*, int, const void*))                                                      \
  X(void, nvnSamplerPoolFinalize, (void*))                                                                               \
  X(bool, nvnProgramInitialize, (void*, void*))                                                                          \
  X(bool, nvnProgramSetShaders, (void*, int, const void*))                                                               \
  X(void, nvnProgramFinalize, (void*))                                                                                   \
  X(void, nvnWindowBuilderSetDefaults, (void*))                                                                          \
  X(void, nvnWindowBuilderSetDevice, (void*, void*))                                                                     \
  X(void, nvnWindowBuilderSetNativeWindow, (void*, void*))                                                               \
  X(void, nvnWindowBuilderSetTextures, (void*, int, void* const*))                                                       \
  X(void, nvnWindowBuilderSetNumActiveTextures, (void*, int))                                                            \
  X(bool, nvnWindowInitialize, (void*, const void*))                                                                     \
  X(void, nvnWindowFinalize, (void*))                                                                                    \
  X(int, nvnWindowAcquireTexture, (void*, void*, int*))                                                                  \
  X(bool, nvnCommandBufferInitialize, (void*, void*))                                                                    \
  X(void, nvnCommandBufferFinalize, (void*))                                                                             \
  X(void, nvnCommandBufferBeginRecording, (void*))                                                                       \
  X(uint64_t, nvnCommandBufferEndRecording, (void*))                                                                     \
  X(void, nvnCommandBufferSetRenderTargets, (void*, int, void* const*, const void* const*, void*, const void*))          \
  X(void, nvnCommandBufferSetViewport, (void*, int, int, int, int))                                                      \
  X(void, nvnCommandBufferSetScissor, (void*, int, int, int, int))                                                       \
  X(void, nvnCommandBufferClearColor, (void*, int, const float*, int))                                                   \
  X(void, nvnCommandBufferClearColori, (void*, int, const int32_t*, int))                                                 \
  X(void, nvnCommandBufferClearColorui, (void*, int, const uint32_t*, int))                                               \
  X(void, nvnCommandBufferClearDepthStencil, (void*, float, bool, int, int))                                             \
  X(void, nvnCommandBufferSetStencilRef, (void*, int, int))                                                             \
  X(void, nvnCommandBufferBindProgram, (void*, const void*, int))                                                        \
  X(void, nvnCommandBufferBindVertexAttribState, (void*, int, const void*))                                              \
  X(void, nvnCommandBufferBindVertexStreamState, (void*, int, const void*))                                              \
  X(void, nvnCommandBufferBindVertexBuffer, (void*, int, uint64_t, int64_t))                                             \
  X(void, nvnCommandBufferBindUniformBuffer, (void*, int, int, uint64_t, int64_t))                                       \
  X(void, nvnCommandBufferBindSeparateTexture, (void*, int, int, uint64_t))                                              \
  X(void, nvnCommandBufferBindSeparateSampler, (void*, int, int, uint64_t))                                              \
  X(void, nvnCommandBufferSetTexturePool, (void*, const void*))                                                          \
  X(void, nvnCommandBufferSetSamplerPool, (void*, const void*))                                                          \
  X(void, nvnCommandBufferDrawArrays, (void*, int, int, int))                                                            \
  X(void, nvnCommandBufferDrawArraysIndirect, (void*, int, uint64_t))                                                  \
  X(void, nvnCommandBufferDrawElementsIndirect, (void*, int, int, uint64_t, uint64_t))                                   \
  X(void, nvnCommandBufferDrawArraysInstanced, (void*, int, int, int, int, int))                                          \
  X(void, nvnCommandBufferBindBlendState, (void*, const void*))                                                          \
  X(void, nvnCommandBufferBindColorState, (void*, const void*))                                                          \
  X(void, nvnCommandBufferBindChannelMaskState, (void*, const void*))                                                    \
  X(void, nvnCommandBufferBindDepthStencilState, (void*, const void*))                                                   \
  X(void, nvnCommandBufferBindMultisampleState, (void*, const void*))                                                    \
  X(void, nvnCommandBufferBindPolygonState, (void*, const void*))                                                        \
  X(void, nvnCommandBufferBarrier, (void*, int))                                                                         \
  X(void, nvnCommandBufferCopyBufferToBuffer, (void*, uint64_t, uint64_t, int64_t, int))                                 \
  X(void, nvnCommandBufferCopyTextureToBuffer, (void*, void*, const void*, const void*, uint64_t, int))                  \
  X(void, nvnCommandBufferCopyBufferToTexture, (void*, uint64_t, void*, const void*, const void*, int))                  \
  X(void, nvnCommandBufferCopyTextureToTexture, (void*, void*, const void*, const void*, void*, const void*, const void*, int))   \
  X(void, nvnCommandBufferDownsample, (void*, void*, void*))                                                            \
  X(void, nvnCommandBufferDownsampleTextureView, (void*, void*, const void*, void*, const void*))                      \
  X(void, nvnCommandBufferSetSampleMask, (void*, int))                                                                  \
  X(int64_t, nvnCommandBufferGetCopyRowStride, (const void*))                                                            \
  X(int64_t, nvnCommandBufferGetCopyImageStride, (const void*))                                                          \
  X(void, nvnCommandBufferSetCopyRowStride, (void*, int64_t))                                                            \
  X(void, nvnCommandBufferSetCopyImageStride, (void*, int64_t))                                                          \
  X(void, nvnCommandBufferFenceSync, (void*, void*, int, int)) \
  X(void, nvnCommandBufferUpdateUniformBuffer, (void*, uint64_t, int64_t, int64_t, int64_t, const void*))                \
  X(void, nvnBlendStateSetDefaults, (void*))                                                                             \
  X(void, nvnColorStateSetDefaults, (void*))                                                                             \
  X(void, nvnChannelMaskStateSetDefaults, (void*))                                                                       \
  X(void, nvnDepthStencilStateSetDefaults, (void*))                                                                      \
  X(void, nvnDepthStencilStateSetDepthTestEnable, (void*, bool))                                                         \
  X(void, nvnDepthStencilStateSetDepthWriteEnable, (void*, bool))                                                        \
  X(void, nvnDepthStencilStateSetDepthFunc, (void*, int))                                                                \
  X(void, nvnDepthStencilStateSetStencilTestEnable, (void*, bool))                                                       \
  X(void, nvnDepthStencilStateSetStencilFunc, (void*, int, int))                                                        \
  X(void, nvnMultisampleStateSetDefaults, (void*))                                                                       \
  X(void, nvnPolygonStateSetDefaults, (void*))                                                                           \
  X(void, nvnPolygonStateSetCullFace, (void*, int))                                                                      \
  X(void, nvnVertexAttribStateSetDefaults, (void*))                                                                      \
  X(void, nvnVertexAttribStateSetFormat, (void*, int, int64_t))                                                          \
  X(void, nvnVertexAttribStateSetStreamIndex, (void*, int))                                                              \
  X(void, nvnVertexStreamStateSetDefaults, (void*))                                                                      \
  X(void, nvnVertexStreamStateSetDivisor, (void*, int))                                                                  \
  X(int, nvnVertexStreamStateGetDivisor, (const void*))                                                                  \
  X(void, nvnVertexStreamStateSetStride, (void*, int64_t))

#define DECL(ret, name, args) static ret(*name) args = nullptr;
PROCS(DECL)
#undef DECL
static bool LoadProcs() {
  bool ok = true;
#define LOAD(ret, name, args)                                                  \
  name = (ret(*) args)ResolveProc(#name);                                      \
  if (!name) { printf("proc %s did not resolve\n", #name); ok = false; }
  PROCS(LOAD)
#undef LOAD
  return ok;
}

// ---- test helpers -------------------------------------------------------------------------------------------------------
static std::string g_outDir = ".";
static void* Alloc(size_t n) { void* p = _aligned_malloc(n, 0x1000); memset(p, 0, n); return p; }

struct GuestPool { alignas(16) uint8_t blob[0x100 + 64]; uint8_t* mem = nullptr; size_t size = 0, bump = 0; uint64_t va = 0; };
static bool MakePool(GuestPool* gp, void* dev, size_t size, int flags) {
  alignas(16) uint8_t b[0x80]; memset(b, 0, sizeof b);
  gp->mem = (uint8_t*)Alloc(size); gp->size = size; gp->bump = 0;
  nvnMemoryPoolBuilderSetDefaults(b); nvnMemoryPoolBuilderSetDevice(b, dev); nvnMemoryPoolBuilderSetFlags(b, flags);
  nvnMemoryPoolBuilderSetStorage(b, gp->mem, size);
  memset(gp->blob, 0xAB, sizeof gp->blob);
  if (!nvnMemoryPoolInitialize(gp->blob, b)) return false;
  gp->va = nvnMemoryPoolGetBufferAddress(gp->blob);
  return true;
}
static uint64_t Bump(GuestPool* gp, uint64_t size, uint64_t align = 0x1000) {
  gp->bump = (gp->bump + align - 1) & ~(align - 1);
  uint64_t o = gp->bump; gp->bump += size;
  if (gp->bump > gp->size) { printf("pool exhausted\n"); abort(); }
  return o;
}

struct GuestTex {
  alignas(16) uint8_t blob[0x80 + 0x40];     // the NVNtexture is 0x80 bytes; the tail is a canary
  TextureG* g() { return (TextureG*)blob; }
  uint64_t off = 0;
  bool ok = false;
};
static bool MakeTex(GuestTex* t, GuestPool* pool, void* dev, int format, int w, int h, int flags,
                    int target = NVN_TEXTURE_TARGET_2D, int depth = 1, int samples = 0, int levels = 1) {
  alignas(16) uint8_t b[0x100]; memset(b, 0, sizeof b);
  nvnTextureBuilderSetDefaults(b); nvnTextureBuilderSetDevice(b, dev); nvnTextureBuilderSetTarget(b, target);
  nvnTextureBuilderSetSize3D(b, w, h, depth); nvnTextureBuilderSetFormat(b, format); nvnTextureBuilderSetFlags(b, flags);
  if (samples) nvnTextureBuilderSetSamples(b, samples);
  if (levels != 1) nvnTextureBuilderSetLevels(b, levels);
  int64_t size = nvnTextureBuilderGetStorageSize(b), align = nvnTextureBuilderGetStorageAlignment(b);
  t->off = Bump(pool, (uint64_t)size, (uint64_t)std::max<int64_t>(align, 0x1000));
  nvnTextureBuilderSetStorage(b, pool->blob, (int64_t)t->off);
  memset(t->blob, 0xCD, sizeof t->blob);
  memset(t->blob, 0, 0x80);
  t->ok = nvnTextureInitialize(t->blob, b);
  return t->ok;
}
static bool CanaryIntact(const GuestTex& t) {
  for (size_t i = 0x80; i < sizeof t.blob; i++) if (t.blob[i] != 0xCD) return false;
  return true;
}

struct Shader { std::vector<uint32_t> spv; };
static std::string ReadWhole(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return ""; std::string s; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n); fclose(f); return s; }
static bool Compile(const std::string& glslc, const std::string& src, const std::string& out, std::vector<uint32_t>* spv) {
  std::string cmd = "\"\"" + glslc + "\" --target-env=vulkan1.3 -O0 \"" + src + "\" -o \"" + out + "\"\"";
  int rc = system(cmd.c_str());
  std::string bytes = ReadWhole(out);
  if (rc != 0 || bytes.size() < 20 || bytes.size() % 4) { printf("glslc failed on %s (rc %d)\n", src.c_str(), rc); return false; }
  spv->resize(bytes.size() / 4); memcpy(spv->data(), bytes.data(), bytes.size());
  return true;
}

struct Px { uint8_t r, g, b, a; };
static Px At(const std::vector<uint8_t>& img, int w, int x, int y) { const uint8_t* p = &img[((size_t)y * w + x) * 4]; return {p[0], p[1], p[2], p[3]}; }
static bool Near(Px a, Px b, int tol = 1) {
  return std::abs((int)a.r - b.r) <= tol && std::abs((int)a.g - b.g) <= tol && std::abs((int)a.b - b.b) <= tol && std::abs((int)a.a - b.a) <= tol;
}
static const char* S(Px p) { static char buf[4][32]; static int i = 0; i = (i + 1) & 3; snprintf(buf[i], 32, "(%d,%d,%d,%d)", p.r, p.g, p.b, p.a); return buf[i]; }
static bool WritePpm(const std::string& path, const std::vector<uint8_t>& img, int w, int h) {
  FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) fwrite(&img[(size_t)i * 4], 1, 3, f);
  fclose(f); return true;
}

// ---- the test ----------------------------------------------------------------------------------------------------------
struct Vtx { float x, y, z, u, v; };

int main(int argc, char** argv) {
  bool wantPresent = true, wantValidation = true;
  setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--no-present")) wantPresent = false;
    else if (!strcmp(argv[i], "--no-validation")) wantValidation = false;
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) g_outDir = argv[++i];
  }
  if (const char* o = getenv("NVN_OUT_DIR")) g_outDir = o;
  std::string testDir = getenv("NVN_TEST_DIR") ? getenv("NVN_TEST_DIR") : "";
  if (testDir.empty()) { std::string f = __FILE__; size_t s = f.find_last_of("/\\"); testDir = s == std::string::npos ? "." : f.substr(0, s); }
  std::string glslc = getenv("NVN_GLSLC") ? getenv("NVN_GLSLC") : "A:/AI/Recomps/new-pokemon-snap/tools/vulkan_sdk/Bin/glslc.exe";
  std::string layerPath = getenv("NVN_LAYER_PATH") ? getenv("NVN_LAYER_PATH") : "A:/AI/Recomps/new-pokemon-snap/tools/vulkan_sdk/Bin";
  CreateDirectoryA(g_outDir.c_str(), nullptr);

  if (!LoadProcs()) return 1;

  // shaders (GLSL of this test, compiled with the SDK's glslc)
  Shader vs, fsTex, fsFlat;
  if (!Compile(glslc, testDir + "/shaders/quad.vert", g_outDir + "/quad.vert.spv", &vs.spv) ||
      !Compile(glslc, testDir + "/shaders/tex.frag", g_outDir + "/tex.frag.spv", &fsTex.spv) ||
      !Compile(glslc, testDir + "/shaders/flat.frag", g_outDir + "/flat.frag.spv", &fsFlat.spv)) { printf("NOT_RUN: glslc unavailable\n"); return 3; }

  VkInitOptions vo; vo.validation = wantValidation; vo.layerPath = layerPath; vo.surfaceExtensions = wantPresent;
  Configure(vo);
  SetWindowHidden(true);

  alignas(16) static uint8_t devB[0x80], dev[0x3000 + 0x40], qB[0x80], queue[0x2000 + 0x40];
  nvnDeviceBuilderSetDefaults(devB); nvnDeviceBuilderSetFlags(devB, 0);
  if (!nvnDeviceInitialize(dev, devB)) { printf("NOT_RUN: device initialize failed: %s\n", LastVkError().c_str()); return 3; }
  printf("device: %s (Vulkan %u.%u.%u)  validation=%s present=%s depthClipControl=%d 4444=%d hostImport=%d\n", g_vk.props.deviceName,
         VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion), VK_API_VERSION_PATCH(g_vk.props.apiVersion),
         g_vk.messenger ? "on" : "off", g_vk.hasSwapchain ? "yes" : "no", g_vk.hasDepthClipControl, g_vk.has4444, g_vk.hasHostImport);
  if (wantValidation) CHECK(g_vk.messenger != VK_NULL_HANDLE, "validation layer was requested but is not active (layer path %s)", layerPath.c_str());
  nvnDeviceSetDepthMode(dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);
  nvnDeviceSetWindowOriginMode(dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
  { int v = 0; nvnDeviceGetInteger(dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &v); CHECK(v == 256, "uniform buffer alignment %d", v); }

  nvnQueueBuilderSetDefaults(qB); nvnQueueBuilderSetDevice(qB, dev);
  CHECK(nvnQueueInitialize(queue, qB), "queue initialize");

  // memory
  GuestPool data, shaderPool;
  CHECK(MakePool(&data, dev, 4u << 20, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED), "data pool");
  VkBuffer checkedBuffer = VK_NULL_HANDLE;
  uint64_t checkedOffset = 0;
  CHECK(!ResolveBuffer(data.va + data.size - 4, UINT64_MAX, &checkedBuffer, &checkedOffset),
        "overflowing buffer range is rejected");
  CHECK(!HostPtr(data.va + data.size - 4, UINT64_MAX), "overflowing host range is rejected");
  CHECK(MakePool(&shaderPool, dev, 0x10000, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_SHADER_CODE), "shader pool");
  CHECK(nvnMemoryPoolMap(data.blob) == data.mem, "pool map returns the storage the guest gave");
  CHECK(data.va != 0 && data.va != shaderPool.va, "pools get distinct GPU addresses");

  // shaders: the program's stage bytes live in a shader pool; the cache key is their SHA-256
  struct ShaderBlob { const char* tag; const Shader* s; int stage; Manifest m; uint64_t off = 0; alignas(16) uint8_t ctl[0x710]; };
  ShaderBlob sb[3];
  sb[0].tag = "NVN-TEST-VS-quad"; sb[0].s = &vs; sb[0].stage = NVN_SHADER_STAGE_VERTEX;
  sb[0].m.set = 0; sb[0].m.inputs = {{0, 0}, {1, 1}}; sb[0].m.outLocs = {0};
  sb[1].tag = "NVN-TEST-FS-tex"; sb[1].s = &fsTex; sb[1].stage = NVN_SHADER_STAGE_FRAGMENT;
  sb[1].m.set = 1;
  { ManifestTex t{}; t.binding = 0; t.dim = 1; t.arrayLen = 1; t.hCbuf = 2; t.hOff = 0x168; t.hasSampler = true; t.sCbuf = 2; t.sOff = 0x568; t.kind = 1; sb[1].m.texs.push_back(t); }
  sb[1].m.ubos.push_back({1, 3, 0, 16});
  sb[2].tag = "NVN-TEST-FS-flat"; sb[2].s = &fsFlat; sb[2].stage = NVN_SHADER_STAGE_FRAGMENT;
  sb[2].m.set = 1; sb[2].m.ubos.push_back({0, 3, 0, 16});
  for (auto& s : sb) {
    uint8_t code[64]; memset(code, 0, sizeof code); strcpy((char*)code, s.tag);
    s.off = Bump(&shaderPool, sizeof code, 256);
    memcpy(shaderPool.mem + s.off, code, sizeof code);
    memset(s.ctl, 0, sizeof s.ctl); *(uint32_t*)s.ctl = 0x98761234u; *(uint32_t*)(s.ctl + 0x704) = sizeof code;
    ShaderCacheAdd(Sha256Hex(code, sizeof code), s.stage, s.s->spv, s.m);
  }
  alignas(16) static uint8_t progTex[0xC0 + 0x40], progFlat[0xC0 + 0x40];
  {
    ShaderData d[2];
    d[0] = {shaderPool.va + sb[0].off, sb[0].ctl}; d[1] = {shaderPool.va + sb[1].off, sb[1].ctl};
    CHECK(nvnProgramInitialize(progTex, dev), "program init"); CHECK(nvnProgramSetShaders(progTex, 2, d), "program (vs + tex fs) set shaders");
    d[1] = {shaderPool.va + sb[2].off, sb[2].ctl};
    CHECK(nvnProgramInitialize(progFlat, dev), "program init"); CHECK(nvnProgramSetShaders(progFlat, 2, d), "program (vs + flat fs) set shaders");
  }

  // render targets and source textures
  GuestTex color, depth, src, rgba4;
  CHECK(MakeTex(&color, &data, dev, NVN_FORMAT_RGBA8, 64, 64, 0), "color target");
  CHECK(MakeTex(&depth, &data, dev, NVN_FORMAT_DEPTH32F, 64, 64, 0), "depth target");
  CHECK(MakeTex(&src, &data, dev, NVN_FORMAT_RGBA8, 2, 2, 0), "2x2 source texture");
  CHECK(MakeTex(&rgba4, &data, dev, NVN_FORMAT_RGBA4, 2, 2, 0), "2x2 RGBA4 texture");
  CHECK(nvnTextureGetWidth(color.blob) == 64 && nvnTextureGetFormat(color.blob) == NVN_FORMAT_RGBA8, "texture getters");
  CHECK(nvnTextureGetStorageSize(color.blob) >= 64 * 64 * 4, "storage size %lld", (long long)nvnTextureGetStorageSize(color.blob));
  CHECK(color.g()->magic == kMagicTexture && color.g()->impl != nullptr, "texture header written");

  // 2x2 RGBA8 source: the guest swizzles with task-11 and writes straight into pool memory (red, green / blue, white)
  {
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    sr_tex_format f{}; CHECK(sr_tex_format_from_nvn(NVN_FORMAT_RGBA8, &f) == SR_TEX_OK, "task-11 knows RGBA8");
    sr_tex_surface_desc d{}; d.format = f; d.width = 2; d.height = 2; d.depth = 1; d.layers = 1; d.levels = 1; d.dim = SR_TEX_DIM_2D;
    d.block_height_log2 = SR_TEX_BH_AUTO; d.block_depth_log2 = SR_TEX_BH_AUTO;
    sr_tex_plan plan; CHECK(sr_tex_plan_build(&d, &plan) == SR_TEX_OK, "plan");
    CHECK(sr_tex_swizzle_level(&plan, 0, 0, texels, 0, 0, data.mem + src.off, (size_t)nvnTextureGetStorageSize(src.blob)) == SR_TEX_OK, "swizzle");
    nvnMemoryPoolFlushMappedRange(data.blob, (int64_t)src.off, (size_t)nvnTextureGetStorageSize(src.blob));
  }
  // 2x2 RGBA4 through NVN's own WriteTexels. NVN names components from the low bits: R is bits 0..3.
  {
    const uint16_t texels[4] = {0x000F, 0x00F0, 0x0F00, 0xF000};     // R, G, B, A
    int region[6] = {0, 0, 0, 2, 2, 1};
    nvnTextureWriteTexels(rgba4.blob, nullptr, region, texels);
  }

  // samplers and the descriptor pools
  alignas(16) static uint8_t sampB[0x80], samp[0x30 + 0x40], texPool[0x20 + 0x40], sampPool[0x20 + 0x40];
  nvnSamplerBuilderSetDefaults(sampB); nvnSamplerBuilderSetDevice(sampB, dev);
  nvnSamplerBuilderSetMinMagFilter(sampB, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
  nvnSamplerBuilderSetWrapMode(sampB, NVN_WRAP_MODE_CLAMP, NVN_WRAP_MODE_CLAMP, NVN_WRAP_MODE_CLAMP);
  CHECK(nvnSamplerInitialize(samp, sampB), "sampler");
  const int kTexSrc = 256, kTexRgba4 = 257, kSamp = 256;      // ids below 256 are reserved on the console
  uint64_t tpOff = Bump(&data, 0x8000), spOff = Bump(&data, 0x2000);
  CHECK(nvnTexturePoolInitialize(texPool, data.blob, (int64_t)tpOff, 512), "texture pool");
  CHECK(nvnSamplerPoolInitialize(sampPool, data.blob, (int64_t)spOff, 512), "sampler pool");
  nvnTexturePoolRegisterTexture(texPool, kTexSrc, src.blob, nullptr);
  nvnTexturePoolRegisterTexture(texPool, kTexRgba4, rgba4.blob, nullptr);
  nvnSamplerPoolRegisterSampler(sampPool, kSamp, samp);
  const uint64_t hSampler = nvnDeviceGetSeparateSamplerHandle(dev, kSamp);

  // buffers: vertices (five quads), uniforms (one 256 byte slot each), a scratch pair for copies
  alignas(16) static uint8_t bB[0x80], vbuf[0x30 + 0x40], ubuf[0x30 + 0x40], cbufA[0x30 + 0x40], cbufB[0x30 + 0x40], cbufC[0x30 + 0x40];
  auto MakeBuf = [&](uint8_t* blob, uint64_t size) {
    uint64_t off = Bump(&data, size, 256);
    nvnBufferBuilderSetDefaults(bB); nvnBufferBuilderSetDevice(bB, dev); nvnBufferBuilderSetStorage(bB, data.blob, (int64_t)off, (int64_t)size);
    bool ok = nvnBufferInitialize(blob, bB); CHECK(ok, "buffer init"); return ok;
  };
  CHECK(MakeBuf(vbuf, 0x1000) && MakeBuf(ubuf, 0x2000) && MakeBuf(cbufA, 256) && MakeBuf(cbufB, 256) && MakeBuf(cbufC, 64 * 64 * 4), "buffers");
  CHECK(nvnBufferGetAddress(vbuf) >= data.va && nvnBufferGetAddress(vbuf) < data.va + data.size, "buffer address inside its pool");
  CHECK((uint8_t*)nvnBufferMap(vbuf) >= data.mem && (uint8_t*)nvnBufferMap(vbuf) < data.mem + data.size, "buffer map inside its pool");

  // quads, each 4 vertices of a triangle strip: (x0,x1) range, z, uv 0..1
  auto Quad = [](Vtx* v, float x0, float x1, float z) {
    v[0] = {x0, 1, z, 0, 0}; v[1] = {x0, -1, z, 0, 1}; v[2] = {x1, 1, z, 1, 0}; v[3] = {x1, -1, z, 1, 1};
  };
  Vtx* vtx = (Vtx*)nvnBufferMap(vbuf);
  Quad(vtx + 0, -1.0f, 1.0f, 0.5f);       // quad 0: the whole target
  Quad(vtx + 4, -1.0f, -0.5f, 0.5f);      // quad 1..4: depth range columns (x 0..15, 16..31, 32..47, 48..63)
  Quad(vtx + 8, -0.5f, 0.0f, -0.5f);
  Quad(vtx + 12, 0.0f, 0.5f, 1.5f);
  Quad(vtx + 16, 0.5f, 1.0f, 0.25f);
  nvnBufferFlushMappedRange(vbuf, 0, 0x1000);
  const uint64_t vbAddr = nvnBufferGetAddress(vbuf), ubAddr = nvnBufferGetAddress(ubuf);
  auto SetUniform = [&](int slot, float r, float g, float b, float a) {
    float* f = (float*)((uint8_t*)nvnBufferMap(ubuf) + 256 * slot); f[0] = r; f[1] = g; f[2] = b; f[3] = a;
  };
  SetUniform(0, 1, 1, 1, 1);
  SetUniform(1, 1, 0, 0, 1); SetUniform(2, 0, 1, 0, 1); SetUniform(3, 0, 0, 1, 1); SetUniform(4, 1, 1, 1, 1);
  nvnBufferFlushMappedRange(ubuf, 0, 0x2000);

  // fixed function state objects, vertex layout
  alignas(16) static uint8_t blend[16], colorSt[16], chan[16], ds[16], ms[0x40], poly[16], attrs[2][4], stream[8];
  nvnBlendStateSetDefaults(blend); nvnColorStateSetDefaults(colorSt); nvnChannelMaskStateSetDefaults(chan);
  nvnDepthStencilStateSetDefaults(ds); nvnDepthStencilStateSetDepthTestEnable(ds, true); nvnDepthStencilStateSetDepthWriteEnable(ds, true);
  nvnDepthStencilStateSetDepthFunc(ds, NVN_DEPTH_FUNC_LESS);
  nvnMultisampleStateSetDefaults(ms); nvnPolygonStateSetDefaults(poly); nvnPolygonStateSetCullFace(poly, NVN_FACE_NONE);
  nvnVertexAttribStateSetDefaults(attrs[0]); nvnVertexAttribStateSetFormat(attrs[0], NVN_FORMAT_RGB32F, 0); nvnVertexAttribStateSetStreamIndex(attrs[0], 0);
  nvnVertexAttribStateSetDefaults(attrs[1]); nvnVertexAttribStateSetFormat(attrs[1], NVN_FORMAT_RG32F, 12); nvnVertexAttribStateSetStreamIndex(attrs[1], 0);
  nvnVertexStreamStateSetDefaults(stream); nvnVertexStreamStateSetStride(stream, sizeof(Vtx));

  alignas(16) static uint8_t cb[0xA0 + 0x40];
  CHECK(nvnCommandBufferInitialize(cb, dev), "command buffer init");
  auto Record = [&](auto&& body) {
    nvnCommandBufferBeginRecording(cb); body(); return nvnCommandBufferEndRecording(cb);
  };
  auto SubmitWait = [&](uint64_t h) { nvnQueueSubmitCommands(queue, 1, &h); nvnQueueFinish(queue); };
  auto Targets = [&](GuestTex& c, bool withDepth) {
    void* rts[1] = {c.blob};
    nvnCommandBufferSetRenderTargets(cb, 1, rts, nullptr, withDepth ? depth.blob : nullptr, nullptr);
    nvnCommandBufferSetViewport(cb, 0, 0, 64, 64); nvnCommandBufferSetScissor(cb, 0, 0, 64, 64);
  };
  auto State = [&]() {
    nvnCommandBufferBindBlendState(cb, blend); nvnCommandBufferBindColorState(cb, colorSt); nvnCommandBufferBindChannelMaskState(cb, chan);
    nvnCommandBufferBindDepthStencilState(cb, ds); nvnCommandBufferBindMultisampleState(cb, ms); nvnCommandBufferBindPolygonState(cb, poly);
    nvnCommandBufferBindVertexAttribState(cb, 2, attrs); nvnCommandBufferBindVertexStreamState(cb, 1, stream);
  };
  auto Read = [&](GuestTex& t, std::vector<uint8_t>* out) {
    uint32_t w = 0, h = 0; bool ok = ReadbackTexture(t.g()->impl, 0, 0, out, &w, &h); CHECK(ok && w && h, "readback"); return ok;
  };
  auto Clear = [&](GuestTex& c, float r, float g, float b, float a) {
    float col[4] = {r, g, b, a};
    uint64_t h = Record([&]() { Targets(c, true); nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0); });
    SubmitWait(h);
  };

  // ---- 1. clear ----------------------------------------------------------------------------------------------------
  {
    Clear(color, 0.25f, 0.5f, 0.75f, 1.0f);
    std::vector<uint8_t> img; Read(color, &img);
    if (img.size() == 64 * 64 * 4) {
      Px p = At(img, 64, 5, 5), q = At(img, 64, 60, 60);
      CHECK(Near(p, {64, 128, 191, 255}, 1) && Near(q, {64, 128, 191, 255}, 1), "clear color read back as %s %s", S(p), S(q));
      // the same bytes through the NVN copy path: CopyTextureToBuffer into a buffer the host reads
      int region[6] = {0, 0, 0, 64, 64, 1};
      uint64_t h = Record([&]() { nvnCommandBufferCopyTextureToBuffer(cb, color.blob, nullptr, region, nvnBufferGetAddress(cbufC), 0); });
      SubmitWait(h);
      CHECK(memcmp(nvnBufferMap(cbufC), img.data(), img.size()) == 0, "CopyTextureToBuffer matches the readback");
      int invalidX[6] = {63, 0, 0, 2, 1, 1};
      int invalidLayer[6] = {0, 0, 1, 1, 1, 1};
      uint64_t invalid = Record([&]() {
        nvnCommandBufferCopyBufferToTexture(cb, nvnBufferGetAddress(cbufC), color.blob, nullptr, invalidX, 0);
        nvnCommandBufferCopyTextureToBuffer(cb, color.blob, nullptr, invalidLayer, nvnBufferGetAddress(cbufC), 0);
      });
      SubmitWait(invalid);
      std::vector<uint8_t> afterInvalid;
      Read(color, &afterInvalid);
      CHECK(afterInvalid == img, "copies outside the texture subresource preserve the image");
      CHECK(memcmp(nvnBufferMap(cbufC), img.data(), img.size()) == 0, "copy from a missing layer preserves the buffer");
    }
  }

  // A masked clear writes selected channels inside the scissor and preserves all other values.
  {
    Clear(color, 0.25f, 0.5f, 0.75f, 1.0f);
    const float clear[4] = {1, 0, 0, 0};
    SubmitWait(Record([&]() {
      Targets(color, true);
      nvnCommandBufferSetScissor(cb, 2, 3, 4, 4);
      nvnCommandBufferClearColor(cb, 0, clear, NVN_CLEAR_COLOR_MASK_R_BIT | NVN_CLEAR_COLOR_MASK_B_BIT);
      nvnCommandBufferSetScissor(cb, 0, 0, 64, 64);
      nvnCommandBufferClearColor(cb, 0, clear, 0);
    }));
    std::vector<uint8_t> img;
    if (Read(color, &img) && img.size() == 64 * 64 * 4) {
      CHECK(Near(At(img, 64, 2, 3), {255, 128, 0, 255}, 1), "partial clear preserves green and alpha");
      CHECK(Near(At(img, 64, 1, 3), {64, 128, 191, 255}, 1), "partial and zero-mask clears preserve pixels outside the scissor");
    }
  }

  // Integer clears use the declared numeric type and only write the layers selected by the view.
  for (int format : {NVN_FORMAT_RGBA8I, NVN_FORMAT_RGBA8UI}) {
    GuestTex integers;
    const bool ready = MakeTex(&integers, &data, dev, format, 4, 4, 0, NVN_TEXTURE_TARGET_2D_ARRAY, 3);
    CHECK(ready, "integer clear target for format %d", format);
    if (!ready) continue;
    TextureView layers{};
    layers.flags = 16; layers.minLayer = 1; layers.numLayers = 2;
    const int all[6] = {0, 0, 0, 4, 4, 3};
    constexpr size_t layerBytes = 4 * 4 * 4;
    memset(nvnBufferMap(cbufA), 0x2a, 3 * layerBytes);
    nvnBufferFlushMappedRange(cbufA, 0, 3 * layerBytes);
    const int mask = NVN_CLEAR_COLOR_MASK_R_BIT | NVN_CLEAR_COLOR_MASK_A_BIT;
    SubmitWait(Record([&]() {
      nvnCommandBufferCopyBufferToTexture(cb, nvnBufferGetAddress(cbufA), integers.blob, nullptr, all, 0);
      void* targets[] = {integers.blob};
      const void* views[] = {&layers};
      nvnCommandBufferSetRenderTargets(cb, 1, targets, views, nullptr, nullptr);
      nvnCommandBufferSetScissor(cb, 1, 1, 2, 2);
      if (format == NVN_FORMAT_RGBA8I) {
        const int32_t value[] = {-22, 0, 0, -16};
        nvnCommandBufferClearColori(cb, 0, value, mask);
      } else {
        const uint32_t value[] = {234, 0, 0, 240};
        nvnCommandBufferClearColorui(cb, 0, value, mask);
      }
      nvnCommandBufferCopyTextureToBuffer(cb, integers.blob, nullptr, all, nvnBufferGetAddress(cbufB), 0);
    }));
    const auto* pixels = static_cast<const uint8_t*>(nvnBufferMap(cbufB));
    constexpr uint8_t before[] = {42, 42, 42, 42}, cleared[] = {234, 42, 42, 240};
    constexpr size_t pixelOffset = (4 + 1) * 4;
    CHECK(memcmp(pixels + pixelOffset, before, sizeof before) == 0 &&
          memcmp(pixels + layerBytes + pixelOffset, cleared, sizeof cleared) == 0 &&
          memcmp(pixels + 2 * layerBytes + pixelOffset, cleared, sizeof cleared) == 0 &&
          memcmp(pixels + 2 * layerBytes, before, sizeof before) == 0,
          "masked integer clear preserves unselected layers, channels, and pixels (format %d)", format);
    nvnTextureFinalize(integers.blob);
  }

  {
    GuestTex stencil;
    const bool ready = MakeTex(&stencil, &data, dev, NVN_FORMAT_STENCIL8, 8, 8, 0, NVN_TEXTURE_TARGET_2D_ARRAY, 3);
    CHECK(ready, "stencil array for a partial clear");
    if (ready) {
      constexpr size_t layerBytes = 8 * 8;
      memset(nvnBufferMap(cbufA), 0xa5, 3 * layerBytes);
      nvnBufferFlushMappedRange(cbufA, 0, 3 * layerBytes);
      TextureView layers{};
      layers.flags = 16; layers.minLayer = 1; layers.numLayers = 2;
      const int all[6] = {0, 0, 0, 8, 8, 3};
      SubmitWait(Record([&]() {
        nvnCommandBufferCopyBufferToTexture(cb, nvnBufferGetAddress(cbufA), stencil.blob, nullptr, all, 0);
        nvnCommandBufferSetRenderTargets(cb, 0, nullptr, nullptr, stencil.blob, &layers);
        nvnCommandBufferSetScissor(cb, 2, 3, 3, 3);
        nvnCommandBufferClearDepthStencil(cb, 0, false, 0x3c, 0x0f);
        nvnCommandBufferSetScissor(cb, 0, 0, 8, 8);
        nvnCommandBufferClearDepthStencil(cb, 0, false, 0, 0);
        nvnCommandBufferCopyTextureToBuffer(cb, stencil.blob, nullptr, all, nvnBufferGetAddress(cbufB), 0);
      }));
      const auto* bytes = static_cast<const uint8_t*>(nvnBufferMap(cbufB));
      constexpr size_t inside = 3 * 8 + 2;
      CHECK(bytes[inside] == 0xa5 && bytes[layerBytes + inside] == 0xac &&
            bytes[2 * layerBytes + inside] == 0xac && bytes[layerBytes + inside - 1] == 0xa5,
            "stencil mask preserves upper bits, unselected layers, and pixels outside the scissor");
      nvnTextureFinalize(stencil.blob);
    }
  }

  {
    VkFormatProperties properties{};
    g_vk.f.vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &properties);
    if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      GuestTex combined;
      const bool ready = MakeTex(&combined, &data, dev, NVN_FORMAT_DEPTH32F_STENCIL8, 64, 64, 0);
      CHECK(ready, "combined depth and stencil target for a partial stencil clear");
      if (ready) {
        alignas(16) uint8_t stencilOnly[sizeof ds]{};
        nvnDepthStencilStateSetDefaults(stencilOnly);
        nvnDepthStencilStateSetDepthTestEnable(stencilOnly, false);
        nvnDepthStencilStateSetDepthWriteEnable(stencilOnly, false);
        nvnDepthStencilStateSetStencilTestEnable(stencilOnly, true);
        nvnDepthStencilStateSetStencilFunc(stencilOnly, NVN_FACE_FRONT_AND_BACK, NVN_STENCIL_FUNC_EQUAL);
        const int all[6] = {0, 0, 0, 64, 64, 1};
        SubmitWait(Record([&]() {
          Targets(color, false);
          void* targets[] = {color.blob};
          nvnCommandBufferSetRenderTargets(cb, 1, targets, nullptr, combined.blob, nullptr);
          State();
          const float black[4] = {};
          nvnCommandBufferClearColor(cb, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
          nvnCommandBufferClearDepthStencil(cb, 0.75f, true, 0xa5, 0xff);
          nvnCommandBufferSetScissor(cb, 2, 3, 4, 4);
          nvnCommandBufferClearDepthStencil(cb, 0.25f, true, 0x3c, 0x0f);
          nvnCommandBufferSetScissor(cb, 0, 0, 64, 64);
          nvnCommandBufferBindDepthStencilState(cb, stencilOnly);
          nvnCommandBufferSetStencilRef(cb, NVN_FACE_FRONT_AND_BACK, 0xac);
          nvnCommandBufferBindProgram(cb, progFlat, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
          nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 256, 16);
          nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr, 4 * sizeof(Vtx));
          nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
          nvnCommandBufferCopyTextureToBuffer(cb, combined.blob, nullptr, all, nvnBufferGetAddress(cbufC), 0);
        }));
        const auto* depths = static_cast<const float*>(nvnBufferMap(cbufC));
        CHECK(depths[3 * 64 + 2] == 0.25f && depths[3 * 64 + 1] == 0.75f,
              "combined clear updates depth only inside its scissor");
        std::vector<uint8_t> img;
        if (Read(color, &img) && img.size() == 64 * 64 * 4) {
          CHECK(Near(At(img, 64, 2, 3), {255, 0, 0, 255}) && Near(At(img, 64, 1, 3), {0, 0, 0, 0}),
                "following draw sees the combined clear's masked stencil value");
        }
        nvnTextureFinalize(combined.blob);
      }
    }
  }

  {
    GuestTex msaa, resolved;
    const bool msReady = MakeTex(&msaa, &data, dev, NVN_FORMAT_RGBA8, 8, 8, 0, NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY, 3, 4);
    const bool outReady = MakeTex(&resolved, &data, dev, NVN_FORMAT_RGBA8, 8, 8, 0, NVN_TEXTURE_TARGET_2D_ARRAY, 3);
    CHECK(msReady && outReady, "multisample and resolve array textures");
    if (msReady && outReady) {
      constexpr size_t layerBytes = 8 * 8 * 4;
      const uint64_t buffer = nvnBufferGetAddress(cbufC);
      memset(nvnBufferMap(cbufC), 0, 3 * layerBytes);
      nvnBufferFlushMappedRange(cbufC, 0, 3 * layerBytes);
      const int all[6] = {0, 0, 0, 8, 8, 3};
      const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
      alignas(16) uint8_t noDepth[sizeof ds]{};
      nvnDepthStencilStateSetDefaults(noDepth);
      nvnDepthStencilStateSetDepthTestEnable(noDepth, false);
      nvnDepthStencilStateSetDepthWriteEnable(noDepth, false);
      SubmitWait(Record([&]() {
        nvnCommandBufferCopyBufferToTexture(cb, buffer, resolved.blob, nullptr, all, 0);
        nvnCommandBufferSetScissor(cb, 0, 0, 8, 8);
        for (int i = 0; i < 3; i++) {
          TextureView layer{}; layer.flags = 16; layer.minLayer = i; layer.numLayers = 1;
          void* targets[] = {msaa.blob};
          const void* views[] = {&layer};
          nvnCommandBufferSetRenderTargets(cb, 1, targets, views, nullptr, nullptr);
          nvnCommandBufferClearColor(cb, 0, colors[i], NVN_CLEAR_COLOR_MASK_RGBA);
          if (i == 0) {
            State();
            nvnCommandBufferSetViewport(cb, 0, 0, 8, 8);
            nvnCommandBufferBindDepthStencilState(cb, noDepth);
            nvnCommandBufferBindProgram(cb, progFlat, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
            nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 512, 16);
            nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr, 4 * sizeof(Vtx));
            nvnCommandBufferSetSampleMask(cb, 1);
            nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
            nvnCommandBufferSetSampleMask(cb, -1);
          }
        }
        nvnCommandBufferDownsample(cb, msaa.blob, resolved.blob);
        nvnCommandBufferCopyTextureToBuffer(cb, resolved.blob, nullptr, all, buffer, 0);
      }));
      const auto* pixels = static_cast<const uint32_t*>(nvnBufferMap(cbufC));
      const auto* mixed = reinterpret_cast<const uint8_t*>(pixels);
      CHECK(mixed[0] >= 190 && mixed[0] <= 192 && mixed[1] >= 63 && mixed[1] <= 65 &&
            mixed[2] == 0 && mixed[3] == 255, "resolve averages one green sample and three red samples");
      CHECK(pixels[layerBytes / 4] == 0xff00ff00 && pixels[2 * layerBytes / 4] == 0xffff0000,
            "resolve transfers all array layers");

      GuestTex mipDest;
      const bool mipReady = MakeTex(&mipDest, &data, dev, NVN_FORMAT_RGBA8_SRGB, 16, 16, 0, NVN_TEXTURE_TARGET_2D_ARRAY, 3, 0, 2);
      CHECK(mipReady, "resolve destination with an sRGB base format and a compatible linear view");
      if (mipReady) {
        memset(nvnBufferMap(cbufC), 0, 3 * layerBytes);
        nvnBufferFlushMappedRange(cbufC, 0, 3 * layerBytes);
        TextureView srcView{}; srcView.flags = 16; srcView.minLayer = 1; srcView.numLayers = 2;
        TextureView dstView{}; dstView.flags = 2 | 8 | 16; dstView.format = NVN_FORMAT_RGBA8;
        dstView.minLevel = 1; dstView.numLevels = 1; dstView.numLayers = 2;
        TextureView mip{}; mip.flags = 8; mip.minLevel = 1; mip.numLevels = 1;
        TextureView invalid = dstView; invalid.minLevel = 0;
        SubmitWait(Record([&]() {
          nvnCommandBufferCopyBufferToTexture(cb, buffer, mipDest.blob, &mip, all, 0);
          nvnCommandBufferDownsampleTextureView(cb, msaa.blob, &srcView, mipDest.blob, &invalid);
          nvnCommandBufferDownsampleTextureView(cb, msaa.blob, &srcView, mipDest.blob, &dstView);
          nvnCommandBufferCopyTextureToBuffer(cb, mipDest.blob, &mip, all, buffer, 0);
        }));
        CHECK(pixels[0] == 0xff00ff00 && pixels[layerBytes / 4] == 0xffff0000 && pixels[2 * layerBytes / 4] == 0,
              "resolve writes selected source layers into the destination mip and preserves other layers");
        nvnTextureFinalize(mipDest.blob);
      }
    }
    if (msReady) nvnTextureFinalize(msaa.blob);
    if (outReady) nvnTextureFinalize(resolved.blob);
  }
  {
    VkImageFormatProperties props{};
    const VkResult support = g_vk.f.vkGetPhysicalDeviceImageFormatProperties(
        g_vk.phys, VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &props);
    if (support == VK_SUCCESS && (props.sampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
      GuestTex intMsaa, intResolved;
      const bool srcReady = MakeTex(&intMsaa, &data, dev, NVN_FORMAT_RGBA8UI, 4, 4, 0, NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 1, 4);
      const bool dstReady = MakeTex(&intResolved, &data, dev, NVN_FORMAT_RGBA8UI, 4, 4, 0);
      CHECK(srcReady && dstReady, "integer multisample resolve textures");
      if (srcReady && dstReady) {
        const uint32_t colorValue[4] = {17, 35, 63, 127};
        const int region[6] = {0, 0, 0, 4, 4, 1};
        SubmitWait(Record([&]() {
          void* targets[] = {intMsaa.blob};
          nvnCommandBufferSetRenderTargets(cb, 1, targets, nullptr, nullptr, nullptr);
          nvnCommandBufferSetScissor(cb, 0, 0, 4, 4);
          nvnCommandBufferClearColorui(cb, 0, colorValue, NVN_CLEAR_COLOR_MASK_RGBA);
          nvnCommandBufferDownsample(cb, intMsaa.blob, intResolved.blob);
          nvnCommandBufferCopyTextureToBuffer(cb, intResolved.blob, nullptr, region, nvnBufferGetAddress(cbufC), 0);
        }));
        const uint8_t expected[] = {17, 35, 63, 127};
        CHECK(memcmp(nvnBufferMap(cbufC), expected, sizeof expected) == 0, "integer resolve preserves the exact channel values");
      }
      if (srcReady) nvnTextureFinalize(intMsaa.blob);
      if (dstReady) nvnTextureFinalize(intResolved.blob);
    }
  }

  // Packed pools may legitimately end at the last byte used by a texture transfer.
  {
    Clear(color, 0, 0, 0, 1);
    constexpr uint32_t magenta = 0xffff00ff;
    const uint64_t tailOffset = data.size - 4 * sizeof(uint32_t);
    const uint64_t tail = data.va + tailOffset;
    auto* pixels = reinterpret_cast<uint32_t*>(data.mem + tailOffset);
    std::fill_n(pixels, 4, magenta);
    nvnMemoryPoolFlushMappedRange(data.blob, (int64_t)tailOffset, 4 * sizeof(uint32_t));
    const int square[6] = {0, 0, 0, 2, 2, 1};
    const int neighbor[6] = {2, 0, 0, 2, 2, 1};
    SubmitWait(Record([&]() {
      nvnCommandBufferCopyBufferToTexture(cb, tail + 1, color.blob, nullptr, neighbor, 0);
      nvnCommandBufferCopyBufferToTexture(cb, tail + 4, color.blob, nullptr, neighbor, 0);
      nvnCommandBufferCopyBufferToTexture(cb, tail, color.blob, nullptr, square, 0);
      nvnCommandBufferCopyTextureToBuffer(cb, color.blob, nullptr, neighbor, tail + 4, 0);
    }));
    CHECK(std::all_of(pixels, pixels + 4, [](uint32_t value) { return value == magenta; }),
          "rejected texture readback preserves the pool tail");
    std::vector<uint8_t> img;
    if (Read(color, &img) && img.size() == 64 * 64 * 4) {
      CHECK(Near(At(img, 64, 1, 1), {255, 0, 255, 255}) && Near(At(img, 64, 2, 1), {0, 0, 0, 255}),
            "complete pool-tail upload succeeds and invalid uploads preserve neighboring pixels");
    }
  }
  if (g_vk.feat.features.textureCompressionBC) {
    GuestTex blocks;
    bool ready = MakeTex(&blocks, &data, dev, NVN_FORMAT_RGBA_DXT1, 7, 5, 0);
    CHECK(ready, "BC1 texture for a compressed copy with partial edge blocks");
    if (ready) {
      uint8_t bytes[32];
      for (uint8_t i = 0; i < sizeof bytes; i++) bytes[i] = i;
      memcpy(nvnBufferMap(cbufA), bytes, sizeof bytes);
      nvnBufferFlushMappedRange(cbufA, 0, sizeof bytes);
      const uint64_t tail = data.va + data.size - sizeof bytes;
      const int whole[6] = {0, 0, 0, 7, 5, 1};
      const int unaligned[6] = {1, 0, 0, 6, 5, 1};
      SubmitWait(Record([&]() {
        nvnCommandBufferCopyBufferToTexture(cb, nvnBufferGetAddress(cbufA), blocks.blob, nullptr, whole, 0);
        nvnCommandBufferCopyBufferToTexture(cb, nvnBufferGetAddress(cbufA), blocks.blob, nullptr, unaligned, 0);
        nvnCommandBufferCopyTextureToBuffer(cb, blocks.blob, nullptr, whole, tail + 8, 0);
        nvnCommandBufferCopyTextureToBuffer(cb, blocks.blob, nullptr, whole, tail, 0);
      }));
      CHECK(memcmp(data.mem + data.size - sizeof bytes, bytes, sizeof bytes) == 0,
            "7x5 BC1 copy transfers the four compressed blocks at the pool tail");
      constexpr size_t rowBytes = 16, rowPitch = 24, pitchedBytes = rowPitch + rowBytes;
      const uint64_t pitchedOffset = data.size - pitchedBytes;
      memset(data.mem + pitchedOffset, 0x5a, pitchedBytes);
      nvnMemoryPoolFlushMappedRange(data.blob, (int64_t)pitchedOffset, pitchedBytes);
      SubmitWait(Record([&]() {
        nvnCommandBufferSetCopyRowStride(cb, rowPitch);
        nvnCommandBufferCopyTextureToBuffer(cb, blocks.blob, nullptr, whole, data.va + pitchedOffset, 0);
        nvnCommandBufferSetCopyRowStride(cb, 0);
      }));
      const uint8_t* pitched = data.mem + pitchedOffset;
      CHECK(memcmp(pitched, bytes, rowBytes) == 0 && memcmp(pitched + rowPitch, bytes + rowBytes, rowBytes) == 0 &&
            std::all_of(pitched + rowBytes, pitched + rowPitch, [](uint8_t value) { return value == 0x5a; }),
            "BC1 readback preserves padding between compressed block rows");
      nvnTextureFinalize(blocks.blob);
    }
  }

  for (int target : {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_TEXTURE_TARGET_3D}) {
    GuestTex pitched;
    const bool ready = MakeTex(&pitched, &data, dev, NVN_FORMAT_RGBA8, 2, 2, 0, target, 2);
    CHECK(ready, "pitched transfer texture for target %d", target);
    if (!ready) continue;
    auto* upload = static_cast<uint32_t*>(nvnBufferMap(cbufA));
    std::fill_n(upload, 16, 0);
    const size_t uploadRows[] = {0, 4, 9, 13};
    for (size_t row = 0; row < 4; row++) {
      upload[uploadRows[row]] = 0xff000010 + (uint32_t)row;
      upload[uploadRows[row] + 1] = 0xff000020 + (uint32_t)row;
    }
    nvnBufferFlushMappedRange(cbufA, 0, 16 * sizeof(uint32_t));
    constexpr uint32_t sentinel = 0xabcdabcd;
    constexpr size_t outWords = 12;
    const uint64_t outOffset = data.size - outWords * sizeof(uint32_t);
    auto* out = reinterpret_cast<uint32_t*>(data.mem + outOffset);
    std::fill_n(out, outWords, sentinel);
    nvnMemoryPoolFlushMappedRange(data.blob, (int64_t)outOffset, outWords * sizeof(uint32_t));
    const int all[6] = {0, 0, 0, 2, 2, 2};
    SubmitWait(Record([&]() {
      nvnCommandBufferSetCopyRowStride(cb, 16);
      nvnCommandBufferSetCopyImageStride(cb, 36);
      CHECK(nvnCommandBufferGetCopyRowStride(cb) == 16 && nvnCommandBufferGetCopyImageStride(cb) == 36,
            "recording exposes the current strides");
      nvnCommandBufferCopyBufferToTexture(cb, nvnBufferGetAddress(cbufA), pitched.blob, nullptr, all, 0);
      nvnCommandBufferSetCopyRowStride(cb, 12);
      nvnCommandBufferSetCopyImageStride(cb, 28);
      nvnCommandBufferCopyTextureToBuffer(cb, pitched.blob, nullptr, all, data.va + outOffset + 4, 0);
      nvnCommandBufferCopyTextureToBuffer(cb, pitched.blob, nullptr, all, data.va + outOffset, 0);
      nvnCommandBufferSetCopyRowStride(cb, 0);
      nvnCommandBufferSetCopyImageStride(cb, 0);
    }));
    uint32_t expected[outWords];
    std::fill_n(expected, outWords, sentinel);
    const size_t outputRows[] = {0, 3, 7, 10};
    for (size_t row = 0; row < 4; row++) {
      expected[outputRows[row]] = upload[uploadRows[row]];
      expected[outputRows[row] + 1] = upload[uploadRows[row] + 1];
    }
    CHECK(memcmp(out, expected, sizeof expected) == 0,
          "pitched copies preserve row and slice padding and honor the pool boundary (target %d)", target);
    nvnTextureFinalize(pitched.blob);
  }

  // Whole layers and scaled regions must honor both texture subresource offsets.
  const int copyTargets[] = {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_TEXTURE_TARGET_3D};
  for (int sourceTarget : copyTargets) for (int destinationTarget : copyTargets) {
    GuestTex source, destination;
    bool sourceReady = MakeTex(&source, &data, dev, NVN_FORMAT_RGBA8, 4, 4, 0, sourceTarget, 3);
    bool destinationReady = MakeTex(&destination, &data, dev, NVN_FORMAT_RGBA8, 4, 4, 0, destinationTarget, 3);
    CHECK(sourceReady && destinationReady, "textures for layer or slice copies");
    if (sourceReady && destinationReady) {
      constexpr int pixelsPerLayer = 16, totalPixels = 3 * pixelsPerLayer;
      const uint64_t srcBuffer = nvnBufferGetAddress(cbufA), dstBuffer = nvnBufferGetAddress(cbufB);
      auto* sourcePixels = static_cast<uint32_t*>(nvnBufferMap(cbufA));
      auto* destPixels = static_cast<uint32_t*>(nvnBufferMap(cbufB));
      const uint32_t red = 0xff0000ff, green = 0xff00ff00, blue = 0xffff0000;
      std::fill_n(sourcePixels, pixelsPerLayer, red);
      std::fill_n(sourcePixels + pixelsPerLayer, pixelsPerLayer, green);
      std::fill_n(sourcePixels + 2 * pixelsPerLayer, pixelsPerLayer, blue);
      std::fill_n(destPixels, totalPixels, 0);
      nvnBufferFlushMappedRange(cbufA, 0, totalPixels * sizeof(uint32_t));
      nvnBufferFlushMappedRange(cbufB, 0, totalPixels * sizeof(uint32_t));
      const int all[6] = {0, 0, 0, 4, 4, 3};
      const int sourceLayers[6] = {0, 0, 1, 4, 4, 2};
      const int destLayers[6] = {0, 0, 0, 4, 4, 2};
      const int sourceBlit[6] = {0, 0, 1, 4, 4, 1};
      const int destBlit[6] = {0, 0, 2, 2, 2, 1};
      const int invalidDest[6] = {0, 0, 3, 4, 4, 1};
      const bool blit = sourceTarget == destinationTarget;
      SubmitWait(Record([&]() {
        nvnCommandBufferCopyBufferToTexture(cb, srcBuffer, source.blob, nullptr, all, 0);
        nvnCommandBufferCopyBufferToTexture(cb, dstBuffer, destination.blob, nullptr, all, 0);
        nvnCommandBufferCopyTextureToTexture(cb, source.blob, nullptr, sourceLayers, destination.blob, nullptr, destLayers, 0);
        if (blit) nvnCommandBufferCopyTextureToTexture(cb, source.blob, nullptr, sourceBlit, destination.blob, nullptr, destBlit, 0);
        nvnCommandBufferCopyTextureToTexture(cb, source.blob, nullptr, sourceBlit, destination.blob, nullptr, invalidDest, 0);
        nvnCommandBufferCopyTextureToBuffer(cb, destination.blob, nullptr, all, dstBuffer, 0);
      }));
      CHECK(destPixels[0] == green && destPixels[pixelsPerLayer] == blue,
            "copy reads source layers or slices 1 and 2 into destination 0 and 1 (targets %d, %d)", sourceTarget, destinationTarget);
      CHECK(destPixels[2 * pixelsPerLayer] == (blit ? green : 0) && destPixels[totalPixels - 1] == 0,
            "only a valid blit changes layer or slice 2 (targets %d, %d)", sourceTarget, destinationTarget);
      const int reversedSlices[6] = {0, 0, 0, 4, 4, 3};
      SubmitWait(Record([&]() {
        nvnCommandBufferCopyTextureToTexture(cb, source.blob, nullptr, all, destination.blob, nullptr,
                                            reversedSlices, NVN_COPY_FLAGS_MIRROR_Z_BIT);
        nvnCommandBufferCopyTextureToBuffer(cb, destination.blob, nullptr, all, dstBuffer, 0);
      }));
      CHECK(destPixels[0] == blue && destPixels[pixelsPerLayer] == green && destPixels[2 * pixelsPerLayer] == red,
            "Z mirror reverses layers or slices (targets %d, %d)", sourceTarget, destinationTarget);
      if (blit) {
        std::fill_n(destPixels, totalPixels, 0);
        nvnBufferFlushMappedRange(cbufB, 0, totalPixels * sizeof(uint32_t));
        const int scaledSlices[6] = {0, 0, 0, 2, 2, 3};
        SubmitWait(Record([&]() {
          nvnCommandBufferCopyBufferToTexture(cb, dstBuffer, destination.blob, nullptr, all, 0);
          nvnCommandBufferCopyTextureToTexture(cb, source.blob, nullptr, all, destination.blob, nullptr,
                                              scaledSlices, NVN_COPY_FLAGS_MIRROR_Z_BIT);
          nvnCommandBufferCopyTextureToBuffer(cb, destination.blob, nullptr, all, dstBuffer, 0);
        }));
        CHECK(destPixels[0] == blue && destPixels[pixelsPerLayer] == green &&
              destPixels[2 * pixelsPerLayer] == red && destPixels[totalPixels - 1] == 0,
              "scaled Z mirror reverses layers or slices (target %d)", sourceTarget);

        // Distinct corners in slice 0 make both axis reversals observable independently.
        const uint32_t white = 0xffffffff;
        sourcePixels[0] = red; sourcePixels[1] = green;
        sourcePixels[2] = blue; sourcePixels[3] = white;
        nvnBufferFlushMappedRange(cbufA, 0, 4 * sizeof(uint32_t));
        const int corner[6] = {0, 0, 0, 2, 2, 1};
        for (int size : {2, 4}) {
          const int destCorner[6] = {0, 0, 1, size, size, 1};
          for (int flags : {NVN_COPY_FLAGS_MIRROR_X_BIT, NVN_COPY_FLAGS_MIRROR_Y_BIT}) {
            SubmitWait(Record([&]() {
              nvnCommandBufferCopyBufferToTexture(cb, srcBuffer, source.blob, nullptr, corner, 0);
              nvnCommandBufferCopyTextureToTexture(cb, source.blob, nullptr, corner, destination.blob, nullptr,
                                                  destCorner, flags);
              nvnCommandBufferCopyTextureToBuffer(cb, destination.blob, nullptr, all, dstBuffer, 0);
            }));
            const uint32_t first = flags == NVN_COPY_FLAGS_MIRROR_X_BIT ? green : blue;
            const uint32_t last = flags == NVN_COPY_FLAGS_MIRROR_X_BIT ? blue : green;
            const int oppositeCorner = pixelsPerLayer + (size - 1) * 4 + size - 1;
            CHECK(destPixels[pixelsPerLayer] == first && destPixels[oppositeCorner] == last,
                  "mirror selects the correct corners (target %d, size %d, flags %d)", sourceTarget, size, flags);
          }
        }
      }
    }
    if (sourceReady) nvnTextureFinalize(source.blob);
    if (destinationReady) nvnTextureFinalize(destination.blob);
  }

  // A same-size copy from packed HDR color must encode the destination's sRGB channels.
  {
    GuestTex source, destination;
    const bool sourceReady = MakeTex(&source, &data, dev, NVN_FORMAT_R11G11B10F, 1, 1, 0);
    const bool destinationReady = MakeTex(&destination, &data, dev, NVN_FORMAT_RGBA8_SRGB, 1, 1, 0);
    CHECK(sourceReady && destinationReady, "textures for the packed HDR to sRGB copy");
    if (sourceReady && destinationReady) {
      const uint32_t packedColor = (14u << 6) | (13u << 17) | (15u << 27);  // Linear RGB 0.5, 0.25, 1.0.
      const uint64_t sourceAddress = nvnBufferGetAddress(cbufA);
      const uint64_t destAddress = nvnBufferGetAddress(cbufB);
      memcpy(nvnBufferMap(cbufA), &packedColor, sizeof packedColor);
      nvnBufferFlushMappedRange(cbufA, 0, sizeof packedColor);
      const int pixel[6] = {0, 0, 0, 1, 1, 1};
      SubmitWait(Record([&]() {
        nvnCommandBufferCopyBufferToTexture(cb, sourceAddress, source.blob, nullptr, pixel, 0);
        nvnCommandBufferCopyTextureToTexture(cb, source.blob, nullptr, pixel, destination.blob, nullptr, pixel, 0);
        nvnCommandBufferCopyTextureToBuffer(cb, destination.blob, nullptr, pixel, destAddress, 0);
      }));
      Px actual{};
      memcpy(&actual, nvnBufferMap(cbufB), sizeof actual);
      CHECK(Near(actual, {188, 137, 255, 255}), "HDR copy encodes sRGB with opaque alpha, got %s", S(actual));
    }
    if (sourceReady) nvnTextureFinalize(source.blob);
    if (destinationReady) nvnTextureFinalize(destination.blob);
  }

  // A scissor that starts offscreen must retain its original right and bottom edges.
  {
    uint64_t h = Record([&]() {
      Targets(color, true);
      float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
      nvnCommandBufferClearColor(cb, 0, red, NVN_CLEAR_COLOR_MASK_RGBA);
      nvnCommandBufferSetScissor(cb, -8, -4, 16, 12);
      nvnCommandBufferClearColor(cb, 0, green, NVN_CLEAR_COLOR_MASK_RGBA);
      nvnCommandBufferSetScissor(cb, -16, 32, 8, 8);
      nvnCommandBufferClearColor(cb, 0, green, NVN_CLEAR_COLOR_MASK_RGBA);
    });
    SubmitWait(h);
    std::vector<uint8_t> img; Read(color, &img);
    if (img.size() == 64 * 64 * 4) {
      CHECK(Near(At(img, 64, 7, 7), {0, 255, 0, 255}), "clipped clear covers the in-bounds corner");
      CHECK(Near(At(img, 64, 8, 7), {255, 0, 0, 255}), "clipped clear preserves pixels past the right edge");
      CHECK(Near(At(img, 64, 7, 8), {255, 0, 0, 255}), "clipped clear preserves pixels past the bottom edge");
      CHECK(Near(At(img, 64, 0, 32), {255, 0, 0, 255}), "fully offscreen clear leaves the target untouched");
    }
  }

  // ---- 2. textured quad --------------------------------------------------------------------------------------------
  {
    uint64_t h = Record([&]() {
      Targets(color, true); State();
      float col[4] = {0, 0, 0, 0};
      nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
      nvnCommandBufferSetTexturePool(cb, texPool); nvnCommandBufferSetSamplerPool(cb, sampPool);
      nvnCommandBufferBindProgram(cb, progTex, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
      nvnCommandBufferBindSeparateTexture(cb, NVN_SHADER_STAGE_FRAGMENT, 0, nvnDeviceGetSeparateTextureHandle(dev, kTexSrc));
      nvnCommandBufferBindSeparateSampler(cb, NVN_SHADER_STAGE_FRAGMENT, 0, hSampler);
      nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 0, 16);
      nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr + 0, 4 * sizeof(Vtx));
      nvnCommandBufferSetScissor(cb, 1, 1, INT32_MAX, INT32_MAX);
      nvnCommandBufferBindProgram(cb, progFlat, NVN_SHADER_STAGE_VERTEX_BIT);
      const int otherStages = NVN_SHADER_STAGE_ALL_GRAPHICS_BITS | NVN_SHADER_STAGE_COMPUTE_BIT;
      nvnCommandBufferBindProgram(cb, nullptr, otherStages & ~(NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT));
      nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
    });
    SubmitWait(h);
    std::vector<uint8_t> img; Read(color, &img);
    if (img.size() == 64 * 64 * 4) {
      Px tl = At(img, 64, 16, 16), tr = At(img, 64, 48, 16), bl = At(img, 64, 16, 48), br = At(img, 64, 48, 48);
      CHECK(Near(tl, {255, 0, 0, 255}), "top-left texel red, got %s", S(tl));
      CHECK(Near(tr, {0, 255, 0, 255}), "top-right texel green, got %s", S(tr));
      CHECK(Near(bl, {0, 0, 255, 255}), "bottom-left texel blue, got %s", S(bl));
      CHECK(Near(br, {255, 255, 255, 255}), "bottom-right texel white, got %s", S(br));
      WritePpm(g_outDir + "/textured_quad.ppm", img, 64, 64);
    }
  }

  // A divisor of two shares texcoords between instances zero and one.
  {
    constexpr size_t uvOffset = 32 * sizeof(Vtx);
    const float uv[][2] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
    memcpy((uint8_t*)nvnBufferMap(vbuf) + uvOffset, uv, sizeof uv);
    nvnBufferFlushMappedRange(vbuf, 0, 0x1000);
    alignas(16) uint8_t instanceAttrs[2][sizeof attrs[0]]{}, streams[2][sizeof stream]{}, noDepth[sizeof ds]{};
    memcpy(instanceAttrs[0], attrs[0], sizeof attrs[0]);
    nvnVertexAttribStateSetDefaults(instanceAttrs[1]);
    nvnVertexAttribStateSetFormat(instanceAttrs[1], NVN_FORMAT_RG32F, 0);
    nvnVertexAttribStateSetStreamIndex(instanceAttrs[1], 1);
    memcpy(streams[0], stream, sizeof stream);
    nvnVertexStreamStateSetDefaults(streams[1]);
    nvnVertexStreamStateSetStride(streams[1], sizeof uv[0]);
    nvnVertexStreamStateSetDivisor(streams[1], 2);
    CHECK(nvnVertexStreamStateGetDivisor(streams[1]) == 2, "vertex stream retains its divisor");
    nvnDepthStencilStateSetDefaults(noDepth);
    nvnDepthStencilStateSetDepthTestEnable(noDepth, false);
    nvnDepthStencilStateSetDepthWriteEnable(noDepth, false);
    auto DrawInstances = [&](int base) {
      return Record([&]() {
        Targets(color, true); State();
        float black[4] = {};
        nvnCommandBufferClearColor(cb, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
        nvnCommandBufferBindProgram(cb, progTex, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
        nvnCommandBufferBindSeparateTexture(cb, NVN_SHADER_STAGE_FRAGMENT, 0, nvnDeviceGetSeparateTextureHandle(dev, kTexSrc));
        nvnCommandBufferBindSeparateSampler(cb, NVN_SHADER_STAGE_FRAGMENT, 0, hSampler);
        nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr, 16);
        nvnCommandBufferBindDepthStencilState(cb, noDepth);
        nvnCommandBufferBindVertexAttribState(cb, 2, instanceAttrs);
        nvnCommandBufferBindVertexStreamState(cb, 2, streams);
        nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr, 4 * sizeof(Vtx));
        nvnCommandBufferBindVertexBuffer(cb, 1, vbAddr + uvOffset, sizeof uv);
        nvnCommandBufferDrawArraysInstanced(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4, base, 3);
      });
    };
    const Px colors[] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
    for (int base : {0, 1}) {
      SubmitWait(DrawInstances(base));
      std::vector<uint8_t> img; Read(color, &img);
      if (img.size() == 64 * 64 * 4) {
        Px p = At(img, 64, 32, 32);
        const Px expected = colors[base + (g_vk.maxVertexDivisor >= 2 ? 1 : 2)];
        CHECK(Near(p, expected), "third instance with divisor two, base %d, device max %u, expected %s, got %s",
              base, g_vk.maxVertexDivisor, S(expected), S(p));
      }
    }
  }

  // ---- 2b. texture and sampler pools stay bound across command buffers ---------------------------------------------
  // Unity binds its pools in one command buffer and draws from later ones. On hardware the pool registers belong to the queue,
  // so a draw in a later command buffer still samples the real textures. A replay that starts every command buffer with no pool
  // reads the 1x1 white dummy instead: the Loading text became white boxes and the spinner a gray blob (task-11 follow-up).
  {
    uint64_t hp = Record([&]() { nvnCommandBufferSetTexturePool(cb, texPool); nvnCommandBufferSetSamplerPool(cb, sampPool); });
    SubmitWait(hp);
    uint64_t h = Record([&]() {
      Targets(color, true); State();
      float col[4] = {0, 0, 0, 0};
      nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
      nvnCommandBufferBindProgram(cb, progTex, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);     // note: no SetTexturePool here
      nvnCommandBufferBindSeparateTexture(cb, NVN_SHADER_STAGE_FRAGMENT, 0, nvnDeviceGetSeparateTextureHandle(dev, kTexSrc));
      nvnCommandBufferBindSeparateSampler(cb, NVN_SHADER_STAGE_FRAGMENT, 0, hSampler);
      nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 0, 16);
      nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr + 0, 4 * sizeof(Vtx));
      nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
    });
    SubmitWait(h);
    std::vector<uint8_t> img; Read(color, &img);
    if (img.size() == 64 * 64 * 4) {
      Px tl = At(img, 64, 16, 16), tr = At(img, 64, 48, 16), bl = At(img, 64, 16, 48), br = At(img, 64, 48, 48);
      CHECK(Near(tl, {255, 0, 0, 255}), "pool set in an earlier command buffer: top-left texel red, got %s (white means the pool was dropped)", S(tl));
      CHECK(Near(tr, {0, 255, 0, 255}), "pool set in an earlier command buffer: top-right texel green, got %s", S(tr));
      CHECK(Near(bl, {0, 0, 255, 255}), "pool set in an earlier command buffer: bottom-left texel blue, got %s", S(bl));
    }
  }

  // ---- 2c. primitives Vulkan has no topology for, and the device info the game sizes its binding tables from ---------------
  {
    int v5e = 0, v5f = 0;
    nvnDeviceGetInteger(dev, NVN_DEVICE_INFO_SEPARATE_TEXTURE_BINDINGS_PER_STAGE, &v5e);
    nvnDeviceGetInteger(dev, NVN_DEVICE_INFO_SEPARATE_SAMPLER_BINDINGS_PER_STAGE, &v5f);
    CHECK(v5e == 128 && v5f == 32, "separate texture / sampler bindings per stage %d / %d (a zero leaves the game's binding tables unallocated)", v5e, v5f);
    // QUAD_STRIP: the strip quad (v0, v1, v2, v3) is the same four corners as the triangle strip above, so the picture must match it.
    // QUADS: the same corners in perimeter order (v0, v1, v3, v2).
    Vtx* q = vtx + 20;
    q[0] = vtx[0]; q[1] = vtx[1]; q[2] = vtx[3]; q[3] = vtx[2];
    nvnBufferFlushMappedRange(vbuf, 0, 0x1000);
    for (int mode = 0; mode < 2; mode++) {
      uint64_t h = Record([&]() {
        Targets(color, true); State();
        float col[4] = {0, 0, 0, 0};
        nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
        nvnCommandBufferBindProgram(cb, progTex, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
        nvnCommandBufferBindSeparateTexture(cb, NVN_SHADER_STAGE_FRAGMENT, 0, nvnDeviceGetSeparateTextureHandle(dev, kTexSrc));
        nvnCommandBufferBindSeparateSampler(cb, NVN_SHADER_STAGE_FRAGMENT, 0, hSampler);
        nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 0, 16);
        nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr + (mode ? 20 * sizeof(Vtx) : 0), 4 * sizeof(Vtx));
        nvnCommandBufferDrawArrays(cb, mode ? NVN_DRAW_PRIMITIVE_QUADS : NVN_DRAW_PRIMITIVE_QUAD_STRIP, 0, 4);
      });
      SubmitWait(h);
      std::vector<uint8_t> img; Read(color, &img);
      if (img.size() == 64 * 64 * 4) {
        const char* nm = mode ? "QUADS" : "QUAD_STRIP";
        Px tl = At(img, 64, 16, 16), tr = At(img, 64, 48, 16), bl = At(img, 64, 16, 48), br = At(img, 64, 48, 48);
        CHECK(Near(tl, {255, 0, 0, 255}) && Near(tr, {0, 255, 0, 255}) && Near(bl, {0, 0, 255, 255}) && Near(br, {255, 255, 255, 255}),
              "%s drawn as two triangles: corners %s %s %s %s", nm, S(tl), S(tr), S(bl), S(br));
      }
    }
  }

  // The same index bytes form degenerate triangles at 8 bits and a visible triangle at 16 bits.
  {
    const uint8_t indices[] = {0, 0, 1, 0, 2, 0, 3, 0, 0, 0, 0, 0};
    const VkDrawIndexedIndirectCommand args{6, 1, 0, 0, 0};
    memcpy(nvnBufferMap(cbufA), indices, sizeof indices);
    memcpy(nvnBufferMap(cbufB), &args, sizeof args);
    nvnBufferFlushMappedRange(cbufA, 0, sizeof indices);
    nvnBufferFlushMappedRange(cbufB, 0, sizeof args);
    for (int type : {NVN_INDEX_TYPE_UNSIGNED_BYTE, NVN_INDEX_TYPE_UNSIGNED_SHORT}) {
      if (type == NVN_INDEX_TYPE_UNSIGNED_BYTE && !g_vk.hasIdx8) continue;
      const uint64_t before = GetStats().draws;
      SubmitWait(Record([&]() {
        Targets(color, true); State();
        float black[4] = {};
        nvnCommandBufferClearColor(cb, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
        nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
        nvnCommandBufferBindProgram(cb, progFlat, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
        nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 256, 16);
        nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr, 4 * sizeof(Vtx));
        nvnCommandBufferDrawElementsIndirect(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, type,
                                              nvnBufferGetAddress(cbufA), nvnBufferGetAddress(cbufB));
      }));
      CHECK(GetStats().draws == before + 1, "submitted indexed indirect draw for index type %d", type);
      std::vector<uint8_t> img;
      if (Read(color, &img) && img.size() == 64 * 64 * 4) {
        const Px expected = type == NVN_INDEX_TYPE_UNSIGNED_BYTE ? Px{0, 0, 0, 0} : Px{255, 0, 0, 255};
        CHECK(Near(At(img, 64, 8, 8), expected), "indexed indirect draw reads index type %d", type);
      }
    }

    // A non-indexed argument record fits at the pool boundary. An indexed record at the same address does not.
    const VkDrawIndirectCommand tailArgs{4, 1, 0, 0};
    const uint64_t tailOffset = data.size - sizeof tailArgs;
    const uint64_t tail = data.va + tailOffset;
    memcpy(data.mem + tailOffset, &tailArgs, sizeof tailArgs);
    nvnMemoryPoolFlushMappedRange(data.blob, (int64_t)tailOffset, sizeof tailArgs);
    const auto before = GetStats();
    SubmitWait(Record([&]() {
      Targets(color, true); State();
      nvnCommandBufferBindProgram(cb, progFlat, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
      nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 256, 16);
      nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr, 4 * sizeof(Vtx));
      nvnCommandBufferDrawArraysIndirect(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, tail);
      nvnCommandBufferDrawElementsIndirect(cb, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT,
                                            nvnBufferGetAddress(cbufA), tail);
      nvnCommandBufferDrawArraysIndirect(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, nvnBufferGetAddress(cbufB) + 1);
    }));
    CHECK(GetStats().draws == before.draws + 1 && GetStats().skippedDraws == before.skippedDraws + 2,
          "indirect replay accepts a complete tail record and rejects truncated or misaligned records");
  }

  // ---- 2d. object lifetime: the queue keeps its pools and program across command buffers, and objects die in the game's order ---
  {
    // A pool and a program the queue still points at are finalized: a later draw must not touch them (hardware state would be stale; ours
    // must be empty). The draw has no program, so it is skipped and the target keeps its clear color.
    alignas(16) static uint8_t progTmp[0xC0 + 0x40], texPool2[0x20 + 0x40], sampPool2[0x20 + 0x40];
    uint64_t tp2 = Bump(&data, 0x8000), sp2 = Bump(&data, 0x2000);
    CHECK(nvnTexturePoolInitialize(texPool2, data.blob, (int64_t)tp2, 512), "second texture pool");
    CHECK(nvnSamplerPoolInitialize(sampPool2, data.blob, (int64_t)sp2, 512), "second sampler pool");
    nvnTexturePoolRegisterTexture(texPool2, kTexSrc, src.blob, nullptr);
    nvnSamplerPoolRegisterSampler(sampPool2, kSamp, samp);
    ShaderData d[2] = {{shaderPool.va + sb[0].off, sb[0].ctl}, {shaderPool.va + sb[2].off, sb[2].ctl}};
    CHECK(nvnProgramInitialize(progTmp, dev) && nvnProgramSetShaders(progTmp, 2, d), "temporary program");
    uint64_t hb = Record([&]() {
      nvnCommandBufferSetTexturePool(cb, texPool2); nvnCommandBufferSetSamplerPool(cb, sampPool2);
      nvnCommandBufferBindProgram(cb, progTmp, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
    });
    SubmitWait(hb);
    nvnTexturePoolFinalize(texPool2); nvnSamplerPoolFinalize(sampPool2); nvnProgramFinalize(progTmp);
    uint64_t skippedBefore = GetStats().noProgNull;
    uint64_t h = Record([&]() {
      Targets(color, true); State();
      float col[4] = {0.5f, 0.5f, 0.5f, 1};
      nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
      nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr + 0, 4 * sizeof(Vtx));
      nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
    });
    SubmitWait(h);
    CHECK(GetStats().noProgNull == skippedBefore + 1, "a draw after the bound program was finalized is skipped as 'no program bound' (%llu -> %llu)",
          (unsigned long long)skippedBefore, (unsigned long long)GetStats().noProgNull);
    std::vector<uint8_t> img; Read(color, &img);
    if (img.size() == 64 * 64 * 4) { Px p = At(img, 64, 32, 32); CHECK(Near(p, {128, 128, 128, 255}, 1), "skipped draw left the clear color, got %s", S(p)); }
    // put the real pools back for the tests below
    uint64_t hp = Record([&]() { nvnCommandBufferSetTexturePool(cb, texPool); nvnCommandBufferSetSamplerPool(cb, sampPool); });
    SubmitWait(hp);

    // A clear under a scissor larger than the target (the game uses 16384 x 16384): the clear rectangle is clipped to the render area.
    {
      uint64_t hc = Record([&]() {
        Targets(color, true);
        nvnCommandBufferSetScissor(cb, 0, 0, 16384, 16384);
        float col[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
      });
      SubmitWait(hc);
      std::vector<uint8_t> im; Read(color, &im);
      if (im.size() == 64 * 64 * 4) { Px p = At(im, 64, 63, 63); CHECK(Near(p, {64, 128, 191, 255}, 1), "clear under an oversized scissor reached the corner, got %s", S(p)); }
    }

    // The game copies NVNtexture structs by value (the CRI movie code does) and finalizes one copy: the other keeps a dangling impl pointer. A
    // struct that was never initialized can hold any bytes there. Commands that name either must be skipped, not followed (this crashed the
    // Vulkan driver in vkCmdCopyBufferToImage with a null image at the first movie).
    {
      GuestPool p2; CHECK(MakePool(&p2, dev, 0x20000, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED), "pool for the stale-copy test");
      GuestTex ta; CHECK(MakeTex(&ta, &p2, dev, NVN_FORMAT_RGBA8, 4, 4, 0), "texture for the stale-copy test");
      GuestTex stale, junk;
      memcpy(stale.blob, ta.blob, 0x80); nvnTextureFinalize(ta.blob);               // `stale` still carries the old impl pointer
      memset(junk.blob, 0xAB, sizeof junk.blob);                                      // never initialized: garbage everywhere
      Clear(color, 0.25f, 0.5f, 0.75f, 1.0f);
      int region[6] = {0, 0, 0, 4, 4, 1};
      uint64_t hs = Record([&]() {
        GuestTex* bad[2] = {&stale, &junk};
        for (GuestTex* t : bad) {
          nvnCommandBufferCopyBufferToTexture(cb, nvnBufferGetAddress(cbufC), t->blob, nullptr, region, 0);
          nvnCommandBufferCopyTextureToBuffer(cb, t->blob, nullptr, region, nvnBufferGetAddress(cbufC), 0);
          nvnCommandBufferCopyTextureToTexture(cb, t->blob, nullptr, region, color.blob, nullptr, region, 0);
          nvnCommandBufferCopyTextureToTexture(cb, color.blob, nullptr, region, t->blob, nullptr, region, 0);
        }
      });
      SubmitWait(hs);
      std::vector<uint8_t> im; Read(color, &im);
      if (im.size() == 64 * 64 * 4) { Px p = At(im, 64, 1, 1); CHECK(Near(p, {64, 128, 191, 255}, 1), "copies naming stale or uninitialized textures changed nothing, got %s", S(p)); }
      CHECK(true, "commands on a stale and an uninitialized texture did not crash");
      nvnMemoryPoolFinalize(p2.blob);
    }

    // A pool is finalized before the texture and buffer that live in it (the game does this at scene changes): both finalizes must be harmless.
    GuestPool tmp;
    CHECK(MakePool(&tmp, dev, 0x20000, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED), "temporary memory pool");
    GuestTex tt; CHECK(MakeTex(&tt, &tmp, dev, NVN_FORMAT_RGBA8, 4, 4, 0), "texture in the temporary pool");
    alignas(16) static uint8_t tb[0x30 + 0x40];
    { uint64_t off = Bump(&tmp, 256, 256); nvnBufferBuilderSetDefaults(bB); nvnBufferBuilderSetDevice(bB, dev); nvnBufferBuilderSetStorage(bB, tmp.blob, (int64_t)off, 256); CHECK(nvnBufferInitialize(tb, bB), "buffer in the temporary pool"); }
    nvnMemoryPoolFlushMappedRange(tmp.blob, 0, 256);
    nvnMemoryPoolFinalize(tmp.blob);
    nvnMemoryPoolFlushMappedRange(tmp.blob, 0, 256);          // the game flushes ranges of pools it already finalized
    nvnTextureFinalize(tt.blob); nvnBufferFinalize(tb);
    CHECK(true, "finalizing a texture and a buffer after their pool did not crash");
  }

  // ---- 3. RGBA4 component order ------------------------------------------------------------------------------------
  {
    uint64_t h = Record([&]() {
      Targets(color, true); State();
      float col[4] = {0.5f, 0.5f, 0.5f, 0.5f};
      nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
      nvnCommandBufferSetTexturePool(cb, texPool); nvnCommandBufferSetSamplerPool(cb, sampPool);
      nvnCommandBufferBindProgram(cb, progTex, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
      nvnCommandBufferBindSeparateTexture(cb, NVN_SHADER_STAGE_FRAGMENT, 0, nvnDeviceGetSeparateTextureHandle(dev, kTexRgba4));
      nvnCommandBufferBindSeparateSampler(cb, NVN_SHADER_STAGE_FRAGMENT, 0, hSampler);
      nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 0, 16);
      nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr + 0, 4 * sizeof(Vtx));
      nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
    });
    SubmitWait(h);
    std::vector<uint8_t> img; Read(color, &img);
    if (img.size() == 64 * 64 * 4) {
      Px tl = At(img, 64, 16, 16), tr = At(img, 64, 48, 16), bl = At(img, 64, 16, 48), br = At(img, 64, 48, 48);
      CHECK(Near(tl, {255, 0, 0, 0}), "RGBA4 0x000F is red (R in bits 0..3), got %s", S(tl));
      CHECK(Near(tr, {0, 255, 0, 0}), "RGBA4 0x00F0 is green, got %s", S(tr));
      CHECK(Near(bl, {0, 0, 255, 0}), "RGBA4 0x0F00 is blue, got %s", S(bl));
      CHECK(Near(br, {0, 0, 0, 255}), "RGBA4 0xF000 is alpha (A in bits 12..15), got %s", S(br));
      WritePpm(g_outDir + "/rgba4_quad.ppm", img, 64, 64);
    }
  }

  // ---- 4. depth range ----------------------------------------------------------------------------------------------
  auto DepthRun = [&](int mode, const char* name) {
    nvnDeviceSetDepthMode(dev, mode);
    uint64_t h = Record([&]() {
      Targets(color, true); State();
      float col[4] = {0, 0, 0, 1};
      nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
      nvnCommandBufferBindProgram(cb, progFlat, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
      for (int q = 1; q <= 4; q++) {
        nvnCommandBufferBindUniformBuffer(cb, NVN_SHADER_STAGE_FRAGMENT, 0, ubAddr + 256 * q, 16);
        nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr + q * 4 * sizeof(Vtx), 4 * sizeof(Vtx));
        nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
      }
    });
    SubmitWait(h);
    std::vector<uint8_t> img, dimg; Read(color, &img); Read(depth, &dimg);
    if (img.size() != 64 * 64 * 4 || dimg.size() != 64 * 64 * 4) return;
    auto D = [&](int x, int y) { float f; memcpy(&f, &dimg[((size_t)y * 64 + x) * 4], 4); return f; };
    Px a = At(img, 64, 8, 32), b = At(img, 64, 24, 32), c = At(img, 64, 40, 32), d = At(img, 64, 56, 32);
    float da = D(8, 32), db = D(24, 32), dc = D(40, 32), dd = D(56, 32);
    printf("depth mode %s: colors %s %s %s %s  depths %.4f %.4f %.4f %.4f\n", name, S(a), S(b), S(c), S(d), da, db, dc, dd);
    if (mode == NVN_DEPTH_MODE_NEAR_IS_ZERO) {
      // clip z in [0, w]: z=0.5 and z=0.25 are drawn with depth equal to z; z=-0.5 and z=1.5 are clipped away
      CHECK(Near(a, {255, 0, 0, 255}) && std::fabs(da - 0.5f) < 1e-4f, "NEAR_IS_ZERO z=0.5 drawn with depth 0.5 (color %s depth %.5f)", S(a), da);
      CHECK(Near(b, {0, 0, 0, 255}) && std::fabs(db - 1.0f) < 1e-6f, "NEAR_IS_ZERO z=-0.5 is clipped (color %s depth %.5f)", S(b), db);
      CHECK(Near(c, {0, 0, 0, 255}) && std::fabs(dc - 1.0f) < 1e-6f, "NEAR_IS_ZERO z=1.5 is clipped (color %s depth %.5f)", S(c), dc);
      CHECK(Near(d, {255, 255, 255, 255}) && std::fabs(dd - 0.25f) < 1e-4f, "NEAR_IS_ZERO z=0.25 drawn with depth 0.25 (color %s depth %.5f)", S(d), dd);
    } else {
      // clip z in [-w, w] maps to depth (z + 1) / 2: -0.5 -> 0.25, 0.5 -> 0.75, 0.25 -> 0.625, 1.5 clipped
      CHECK(Near(a, {255, 0, 0, 255}) && std::fabs(da - 0.75f) < 1e-4f, "NEAR_IS_MINUS_W z=0.5 -> depth 0.75 (color %s depth %.5f)", S(a), da);
      CHECK(Near(b, {0, 255, 0, 255}) && std::fabs(db - 0.25f) < 1e-4f, "NEAR_IS_MINUS_W z=-0.5 -> depth 0.25 (color %s depth %.5f)", S(b), db);
      CHECK(Near(c, {0, 0, 0, 255}) && std::fabs(dc - 1.0f) < 1e-6f, "NEAR_IS_MINUS_W z=1.5 clipped (color %s depth %.5f)", S(c), dc);
      CHECK(Near(d, {255, 255, 255, 255}) && std::fabs(dd - 0.625f) < 1e-4f, "NEAR_IS_MINUS_W z=0.25 -> depth 0.625 (color %s depth %.5f)", S(d), dd);
    }
    WritePpm(g_outDir + (mode == NVN_DEPTH_MODE_NEAR_IS_ZERO ? "/depth_zero.ppm" : "/depth_minus_w.ppm"), img, 64, 64);
  };
  DepthRun(NVN_DEPTH_MODE_NEAR_IS_ZERO, "NEAR_IS_ZERO");
  if (g_vk.hasDepthClipControl) DepthRun(NVN_DEPTH_MODE_NEAR_IS_MINUS_W, "NEAR_IS_MINUS_W");
  else printf("depth mode NEAR_IS_MINUS_W: skipped, VK_EXT_depth_clip_control is not available on this device\n");
  nvnDeviceSetDepthMode(dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);

  // ---- 5. sync, copies, uniform update -----------------------------------------------------------------------------
  {
    alignas(16) static uint8_t sync1[0x40 + 0x40], sync2[0x40 + 0x40];
    CHECK(nvnSyncInitialize(sync1, dev) && nvnSyncInitialize(sync2, dev), "sync init");
    uint8_t* a = (uint8_t*)nvnBufferMap(cbufA); uint8_t* b = (uint8_t*)nvnBufferMap(cbufB);
    for (int i = 0; i < 256; i++) { a[i] = (uint8_t)(i * 7 + 3); b[i] = 0; }
    nvnBufferFlushMappedRange(cbufA, 0, 256); nvnBufferFlushMappedRange(cbufB, 0, 256);
    const uint32_t patch[4] = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
    uint64_t h = Record([&]() {
      nvnCommandBufferCopyBufferToBuffer(cb, nvnBufferGetAddress(cbufA), nvnBufferGetAddress(cbufB), 256, 0);
      nvnCommandBufferBarrier(cb, NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT);
      nvnCommandBufferUpdateUniformBuffer(cb, nvnBufferGetAddress(cbufB), 256, 32, 16, patch);
      nvnCommandBufferFenceSync(cb, sync1, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    });
    nvnQueueSubmitCommands(queue, 1, &h);
    nvnQueueFlush(queue);
    int r = nvnSyncWait(sync1, 5ull * 1000000000ull);
    CHECK(r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED, "SyncWait on a command buffer fence returned %d", r);
    nvnQueueFenceSync(queue, sync2, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    CHECK(nvnQueueWaitSync(queue, sync2), "QueueWaitSync");
    r = nvnSyncWait(sync2, 5ull * 1000000000ull);
    CHECK(r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED, "SyncWait on a queue fence returned %d", r);
    nvnQueueFinish(queue);
    CHECK(memcmp(b, a, 32) == 0 && memcmp(b + 48, a + 48, 208) == 0, "CopyBufferToBuffer copied the data around the patch");
    CHECK(memcmp(b + 32, patch, 16) == 0, "UpdateUniformBuffer wrote its 16 bytes");
    nvnSyncFinalize(sync1); nvnSyncFinalize(sync2);
  }

  // ---- 6. window on a hidden surface -------------------------------------------------------------------------------
  if (wantPresent && g_vk.hasSwapchain) {
    GuestTex wt[2];
    CHECK(MakeTex(&wt[0], &data, dev, NVN_FORMAT_RGBA8, 64, 64, NVN_TEXTURE_FLAGS_DISPLAY) && MakeTex(&wt[1], &data, dev, NVN_FORMAT_RGBA8, 64, 64, NVN_TEXTURE_FLAGS_DISPLAY), "window textures");
    alignas(16) static uint8_t wb[0x80], win[0x180 + 0x40], wsync[0x40 + 0x40];
    void* wtex[2] = {wt[0].blob, wt[1].blob};
    nvnWindowBuilderSetDefaults(wb); nvnWindowBuilderSetDevice(wb, dev); nvnWindowBuilderSetNativeWindow(wb, nullptr);
    nvnWindowBuilderSetTextures(wb, 2, wtex); nvnWindowBuilderSetNumActiveTextures(wb, 2);
    CHECK(nvnWindowInitialize(win, wb), "window initialize");
    nvnSyncInitialize(wsync, dev);
    WindowImpl* w = ImplOf<WindowImpl>(win);
    CHECK(w && w->useSwapchain && w->hwnd, "window has a surface");
    if (w && w->hwnd) CHECK(!IsWindowVisible(w->hwnd), "the presentation window must stay hidden");
    for (int frame = 0; frame < 3; frame++) {
      int idx = -1;
      CHECK(nvnWindowAcquireTexture(win, wsync, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS && idx >= 0 && idx < 2, "acquire index %d", idx);
      if (idx < 0 || idx > 1) break;
      float col[4] = {0.0f, 0.5f * (float)frame, 1.0f, 1.0f};
      uint64_t h = Record([&]() { Targets(wt[idx], true); nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0); });
      nvnQueueWaitSync(queue, wsync);
      nvnQueueSubmitCommands(queue, 1, &h);
      nvnQueuePresentTexture(queue, win, idx);
    }
    nvnQueueFinish(queue);
    CHECK(w && w->presents == 3 && w->swap != VK_NULL_HANDLE, "three frames presented through a swapchain (presents %llu)", w ? (unsigned long long)w->presents : 0ull);
    if (w && w->hwnd) CHECK(!IsWindowVisible(w->hwnd), "the presentation window is still hidden after presenting");
    nvnSyncFinalize(wsync); nvnWindowFinalize(win);
    nvnTextureFinalize(wt[0].blob); nvnTextureFinalize(wt[1].blob);
  } else printf("window present: skipped (%s)\n", wantPresent ? "the device cannot present to a surface" : "--no-present");

  // ---- 7. object sizes ---------------------------------------------------------------------------------------------
  CHECK(CanaryIntact(color) && CanaryIntact(depth) && CanaryIntact(src) && CanaryIntact(rgba4), "an NVNtexture operation wrote past its 0x80 bytes");

  // ---- teardown ----------------------------------------------------------------------------------------------------
  nvnQueueFinish(queue);
  nvnCommandBufferFinalize(cb);
  nvnProgramFinalize(progTex); nvnProgramFinalize(progFlat);
  nvnBufferFinalize(vbuf); nvnBufferFinalize(ubuf); nvnBufferFinalize(cbufA); nvnBufferFinalize(cbufB); nvnBufferFinalize(cbufC);
  nvnTexturePoolFinalize(texPool); nvnSamplerPoolFinalize(sampPool); nvnSamplerFinalize(samp);
  nvnTextureFinalize(color.blob); nvnTextureFinalize(depth.blob); nvnTextureFinalize(src.blob); nvnTextureFinalize(rgba4.blob);
  nvnMemoryPoolFinalize(data.blob); nvnMemoryPoolFinalize(shaderPool.blob);
  nvnQueueFinalize(queue);
  nvnDeviceFinalize(dev);

  printf("validation: %d errors, %d warnings\n", g_vk.valErrors.load(), g_vk.valWarnings.load());
  if (g_vk.valErrors > 0) printf("first validation error: %s\n", g_vk.firstValError.c_str());
  CHECK(g_vk.valErrors == 0, "validation layer reported errors");
  printf("nvn_gpu: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
