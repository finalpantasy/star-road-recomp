// Star Road Recomp: NVN shader cache reader test (task-12, shader cache v1). Our own code (D-008).
//
// Reads the translated shader cache (docs/SHADER_CACHE.md section 5) through the same reader the game uses and runs it on the
// machine's Vulkan device. Hidden, muted, no window and no audio:
//   1. static: index.jsonl loads, a sample of stage programs loads with their SPIR-V, manifests and c[1] constants files
//   2. support buffer and program constants end to end: a test fragment shaped like a cache module reads the 1600 byte support
//      buffer (binding 0) and the c[1] uniform block (binding 1); the pixel proves the words the runtime filled in
//   3. variants: a sample of the real (vs, fs, gs) combinations from variants.jsonl becomes NVN programs (through the same
//      ProgramSetShaders path as the game) and is drawn into a render target with the validation layer on
//   4. sweep (no validation): every stage program in the index goes through the device capability check, VkShaderModule and
//      descriptor set layout creation
// and prints the counts. Variants need a stage key the program can find: the test draws stage bytes of its own, so it aliases the
// SHA-256 of those bytes to the cache key (ShaderCacheAlias).
//
// usage: nvn_cache [--cache DIR] [--variants N | --variants-file FILE] [--gs N] [--no-validation] [--no-sweep] [--sweep-only] [--out DIR]
//   exit 0 pass, 1 fail, 3 not run (no cache, no glslc or no usable Vulkan device)
// env:   NVN_LAYER_PATH, NVN_GLSLC, NVN_TEST_DIR, NVN_OUT_DIR, NVN_CACHE_DIR
#include <malloc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nvn_api.h"

using namespace nvn;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                                                                       \
  do {                                                                                                         \
    g_checks++;                                                                                                \
    if (!(cond)) { g_fails++; printf("FAIL line %d  %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

#define PROCS(X)                                                                                                         \
  X(void, nvnDeviceBuilderSetDefaults, (void*))                                                                          \
  X(void, nvnDeviceBuilderSetFlags, (void*, int))                                                                        \
  X(bool, nvnDeviceInitialize, (void*, const void*))                                                                     \
  X(void, nvnDeviceFinalize, (void*))                                                                                    \
  X(void, nvnDeviceSetDepthMode, (void*, int))                                                                           \
  X(void, nvnDeviceSetWindowOriginMode, (void*, int))                                                                    \
  X(uint64_t, nvnDeviceGetSeparateTextureHandle, (const void*, int))                                                     \
  X(uint64_t, nvnDeviceGetSeparateSamplerHandle, (const void*, int))                                                     \
  X(void, nvnQueueBuilderSetDefaults, (void*))                                                                           \
  X(void, nvnQueueBuilderSetDevice, (void*, void*))                                                                      \
  X(bool, nvnQueueInitialize, (void*, const void*))                                                                      \
  X(void, nvnQueueFinalize, (void*))                                                                                     \
  X(void, nvnQueueSubmitCommands, (void*, int, const uint64_t*))                                                         \
  X(void, nvnQueueFinish, (void*))                                                                                       \
  X(void, nvnMemoryPoolBuilderSetDefaults, (void*))                                                                      \
  X(void, nvnMemoryPoolBuilderSetDevice, (void*, void*))                                                                 \
  X(void, nvnMemoryPoolBuilderSetFlags, (void*, int))                                                                    \
  X(void, nvnMemoryPoolBuilderSetStorage, (void*, void*, size_t))                                                        \
  X(bool, nvnMemoryPoolInitialize, (void*, const void*))                                                                 \
  X(void, nvnMemoryPoolFinalize, (void*))                                                                                \
  X(uint64_t, nvnMemoryPoolGetBufferAddress, (const void*))                                                              \
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
  X(void, nvnTextureBuilderSetSize2D, (void*, int, int))                                                                 \
  X(void, nvnTextureBuilderSetFormat, (void*, int))                                                                      \
  X(void, nvnTextureBuilderSetSamples, (void*, int))                                                                     \
  X(void, nvnTextureBuilderSetDepth, (void*, int))                                                                       \
  X(void, nvnTextureBuilderSetFlags, (void*, int))                                                                       \
  X(void, nvnTextureBuilderSetStorage, (void*, void*, int64_t))                                                          \
  X(int64_t, nvnTextureBuilderGetStorageSize, (const void*))                                                             \
  X(int64_t, nvnTextureBuilderGetStorageAlignment, (const void*))                                                        \
  X(bool, nvnTextureInitialize, (void*, const void*))                                                                    \
  X(void, nvnTextureFinalize, (void*))                                                                                   \
  X(void, nvnSamplerBuilderSetDefaults, (void*))                                                                         \
  X(void, nvnSamplerBuilderSetDevice, (void*, void*))                                                                    \
  X(void, nvnSamplerBuilderSetMinMagFilter, (void*, int, int))                                                           \
  X(void, nvnSamplerBuilderSetWrapMode, (void*, int, int, int))                                                          \
  X(void, nvnSamplerBuilderSetCompare, (void*, int, int))                                                                \
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
  X(bool, nvnCommandBufferInitialize, (void*, void*))                                                                    \
  X(void, nvnCommandBufferFinalize, (void*))                                                                             \
  X(void, nvnCommandBufferBeginRecording, (void*))                                                                       \
  X(uint64_t, nvnCommandBufferEndRecording, (void*))                                                                     \
  X(void, nvnCommandBufferSetRenderTargets, (void*, int, void* const*, const void* const*, void*, const void*))          \
  X(void, nvnCommandBufferSetViewport, (void*, int, int, int, int))                                                      \
  X(void, nvnCommandBufferSetScissor, (void*, int, int, int, int))                                                       \
  X(void, nvnCommandBufferClearColor, (void*, int, const float*, int))                                                   \
  X(void, nvnCommandBufferClearDepthStencil, (void*, float, bool, int, int))                                             \
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
  X(void, nvnCommandBufferBindBlendState, (void*, const void*))                                                          \
  X(void, nvnCommandBufferBindColorState, (void*, const void*))                                                          \
  X(void, nvnCommandBufferBindChannelMaskState, (void*, const void*))                                                    \
  X(void, nvnCommandBufferBindDepthStencilState, (void*, const void*))                                                   \
  X(void, nvnCommandBufferBindMultisampleState, (void*, const void*))                                                    \
  X(void, nvnCommandBufferBindPolygonState, (void*, const void*))                                                        \
  X(void, nvnBlendStateSetDefaults, (void*))                                                                             \
  X(void, nvnColorStateSetDefaults, (void*))                                                                             \
  X(void, nvnChannelMaskStateSetDefaults, (void*))                                                                       \
  X(void, nvnDepthStencilStateSetDefaults, (void*))                                                                      \
  X(void, nvnMultisampleStateSetDefaults, (void*))                                                                       \
  X(void, nvnPolygonStateSetDefaults, (void*))                                                                           \
  X(void, nvnPolygonStateSetCullFace, (void*, int))                                                                      \
  X(void, nvnVertexAttribStateSetDefaults, (void*))                                                                      \
  X(void, nvnVertexAttribStateSetFormat, (void*, int, int64_t))                                                          \
  X(void, nvnVertexAttribStateSetStreamIndex, (void*, int))                                                              \
  X(void, nvnVertexStreamStateSetDefaults, (void*))                                                                      \
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

// ---- helpers (the same shapes as nvn_gpu.cpp) ---------------------------------------------------------------------------
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
  alignas(16) uint8_t blob[0x80 + 0x40];
  TextureG* g() { return (TextureG*)blob; }
  uint64_t off = 0;
};
static bool MakeTex(GuestTex* t, GuestPool* pool, void* dev, int format, int w, int h, int flags, int target = NVN_TEXTURE_TARGET_2D, int samples = 0, int layers = 0) {
  alignas(16) uint8_t b[0x100]; memset(b, 0, sizeof b);
  nvnTextureBuilderSetDefaults(b); nvnTextureBuilderSetDevice(b, dev); nvnTextureBuilderSetTarget(b, target);
  nvnTextureBuilderSetSize2D(b, w, h); nvnTextureBuilderSetFormat(b, format); nvnTextureBuilderSetFlags(b, flags);
  if (samples) nvnTextureBuilderSetSamples(b, samples);
  if (layers) nvnTextureBuilderSetDepth(b, layers);
  int64_t size = nvnTextureBuilderGetStorageSize(b), align = nvnTextureBuilderGetStorageAlignment(b);
  t->off = Bump(pool, (uint64_t)size, (uint64_t)std::max<int64_t>(align, 0x1000));
  nvnTextureBuilderSetStorage(b, pool->blob, (int64_t)t->off);
  memset(t->blob, 0, 0x80);
  return nvnTextureInitialize(t->blob, b);
}
static std::string ReadWhole(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb"); if (!f) return "";
  std::string s; char buf[8192]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n); fclose(f); return s;
}
static bool Compile(const std::string& glslc, const std::string& src, const std::string& out, std::vector<uint32_t>* spv) {
  std::string cmd = "\"\"" + glslc + "\" --target-env=vulkan1.3 -O0 \"" + src + "\" -o \"" + out + "\"\"";
  int rc = system(cmd.c_str());
  std::string bytes = ReadWhole(out);
  if (rc != 0 || bytes.size() < 20 || bytes.size() % 4) { printf("glslc failed on %s (rc %d)\n", src.c_str(), rc); return false; }
  spv->resize(bytes.size() / 4); memcpy(spv->data(), bytes.data(), bytes.size());
  return true;
}

// ---- variants.jsonl ---------------------------------------------------------------------------------------------------
struct Variant { std::string shader, vs, fs, gs; };
static std::string JsonStr(const std::string& line, const char* key, size_t from = 0) {
  std::string k = std::string("\"") + key + "\": \"";
  size_t p = line.find(k, from); if (p == std::string::npos) return "";
  p += k.size(); size_t e = line.find('"', p); return e == std::string::npos ? "" : line.substr(p, e - p);
}
static std::vector<Variant> ReadVariants(const std::string& path) {
  std::vector<Variant> out; std::ifstream f(path); std::string line;
  std::set<std::string> seen;
  while (std::getline(f, line)) {
    size_t st = line.find("\"stages\"");
    if (st == std::string::npos) continue;
    Variant v; v.shader = JsonStr(line, "shader");
    v.vs = JsonStr(line, "vs", st); v.fs = JsonStr(line, "fs", st); v.gs = JsonStr(line, "gs", st);
    if (v.vs.empty() || v.fs.empty()) continue;
    if (!seen.insert(v.vs + v.fs + v.gs).second) continue;
    out.push_back(v);
  }
  return out;
}
// Input primitive a geometry shader declares (SPIR-V OpExecutionMode), as an NVN draw primitive; -1 when it declares none.
static int GsDrawPrimitive(const std::vector<uint32_t>& w) {
  for (size_t i = 5; i < w.size();) {
    uint32_t op = w[i] & 0xFFFF, n = w[i] >> 16;
    if (!n) break;
    if (op == 16 && n >= 3) {
      switch (w[i + 2]) {
        case 19: return NVN_DRAW_PRIMITIVE_POINTS;
        case 20: return NVN_DRAW_PRIMITIVE_LINES;
        case 21: return NVN_DRAW_PRIMITIVE_LINES_ADJACENCY;
        case 22: return NVN_DRAW_PRIMITIVE_TRIANGLES;
        case 23: return NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY;
      }
    }
    i += n;
  }
  return -1;
}

struct Px { uint8_t r, g, b, a; };
static Px At(const std::vector<uint8_t>& img, int w, int x, int y) { const uint8_t* p = &img[((size_t)y * w + x) * 4]; return {p[0], p[1], p[2], p[3]}; }
static bool Near(Px a, Px b, int tol = 1) {
  return std::abs((int)a.r - b.r) <= tol && std::abs((int)a.g - b.g) <= tol && std::abs((int)a.b - b.b) <= tol && std::abs((int)a.a - b.a) <= tol;
}

struct Vtx { float x, y, z, u, v; };

int main(int argc, char** argv) {
  bool wantValidation = true, doSweep = true, sweepOnly = false;
  int nVariants = 600, minGs = 60;
  std::string outDir = ".", cacheDir = getenv("NVN_CACHE_DIR") ? getenv("NVN_CACHE_DIR") : "A:/AI/Recomps/smrpg-work/shaders/cache/v1";
  std::string variantsFile;
  setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--no-validation")) wantValidation = false;
    else if (!strcmp(argv[i], "--no-sweep")) doSweep = false;
    else if (!strcmp(argv[i], "--sweep-only")) sweepOnly = true;
    else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cacheDir = argv[++i];
    else if (!strcmp(argv[i], "--variants") && i + 1 < argc) nVariants = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--variants-file") && i + 1 < argc) variantsFile = argv[++i];
    else if (!strcmp(argv[i], "--gs") && i + 1 < argc) minGs = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
  }
  if (const char* o = getenv("NVN_OUT_DIR")) outDir = o;
  std::string testDir = getenv("NVN_TEST_DIR") ? getenv("NVN_TEST_DIR") : "";
  if (testDir.empty()) { std::string f = __FILE__; size_t s = f.find_last_of("/\\"); testDir = s == std::string::npos ? "." : f.substr(0, s); }
  std::string glslc = getenv("NVN_GLSLC") ? getenv("NVN_GLSLC") : "A:/AI/Recomps/new-pokemon-snap/tools/vulkan_sdk/Bin/glslc.exe";
  std::string layerPath = getenv("NVN_LAYER_PATH") ? getenv("NVN_LAYER_PATH") : "A:/AI/Recomps/new-pokemon-snap/tools/vulkan_sdk/Bin";
  CreateDirectoryA(outDir.c_str(), nullptr);

  if (!LoadProcs()) return 1;
  if (!ShaderCacheLoad(cacheDir)) { printf("NOT_RUN: shader cache not found at %s\n", cacheDir.c_str()); return 3; }

  // ---- 1. static: the index and a sample of stage programs -------------------------------------------------------------
  size_t nIndex = ShaderCacheIndexSize();
  size_t nOk = 0, nFailedIndex = 0;
  std::map<int, size_t> byStage;
  std::vector<std::string> okKeys;
  for (size_t i = 0; i < nIndex; i++) {
    std::string key; int stage = 0; bool ok = false;
    ShaderCacheIndexAt(i, &key, &stage, &ok);
    if (ok) { nOk++; byStage[stage]++; okKeys.push_back(key); } else nFailedIndex++;
  }
  printf("cache %s: %zu index entries, %zu ok, %zu failed in the index\n", cacheDir.c_str(), nIndex, nOk, nFailedIndex);
  for (auto& kv : byStage) printf("  stage %-8s %zu\n", StageName(kv.first), kv.second);
  CHECK(nIndex > 0 && nOk > 0, "empty cache");
  {
    size_t sample = 0, magic = 0, withSupport = 0, withConsts = 0, constsLoaded = 0, withCaps = 0, withTypes = 0, constBytes = 0;
    size_t step = std::max<size_t>(1, okKeys.size() / 600);
    for (size_t i = 0; i < okKeys.size(); i += step) {
      StageProgram* sp = ShaderCacheGet(okKeys[i]);
      sample++;
      if (!sp->ok) { CHECK(false, "stage %s did not load: %s", okKeys[i].c_str(), sp->reason.c_str()); continue; }
      if (sp->spv.size() >= 5 && sp->spv[0] == 0x07230203u) magic++;
      if (sp->man.supportBinding >= 0) { withSupport++; CHECK(sp->man.supportBinding == 0 && sp->man.supportSize == 1600, "support buffer binding %d size %u", sp->man.supportBinding, sp->man.supportSize); }
      if (!sp->man.constFile.empty()) { withConsts++; if (!sp->consts.empty()) { constsLoaded++; constBytes += sp->consts.size(); } CHECK(!sp->consts.empty() && sp->consts.size() <= 432, "constants file %s read as %zu bytes", sp->man.constFile.c_str(), sp->consts.size()); }
      if (!sp->man.caps.empty()) withCaps++;
      if (!sp->man.inputs.empty()) withTypes++;
    }
    printf("static sample: %zu stage programs, %zu with SPIR-V magic, %zu with a support buffer, %zu with c[1] constants (%zu loaded), %zu with capabilities, %zu with inputs\n",
           sample, magic, withSupport, withConsts, constsLoaded, withCaps, withTypes);
    CHECK(magic == sample, "SPIR-V magic missing on %zu stage programs", sample - magic);
  }

  // ---- shaders of the test ----------------------------------------------------------------------------------------------
  std::vector<uint32_t> vsSpv, fsSupportSpv;
  if (!sweepOnly) {
    if (!Compile(glslc, testDir + "/shaders/quad.vert", outDir + "/quad.vert.spv", &vsSpv) ||
        !Compile(glslc, testDir + "/shaders/support.frag", outDir + "/support.frag.spv", &fsSupportSpv)) { printf("NOT_RUN: glslc unavailable\n"); return 3; }
  }

  VkInitOptions vo; vo.validation = wantValidation && !sweepOnly; vo.layerPath = layerPath; vo.surfaceExtensions = false;
  Configure(vo);
  SetWindowHidden(true);
  alignas(16) static uint8_t devB[0x80], dev[0x3000 + 0x40], qB[0x80], queue[0x2000 + 0x40];
  nvnDeviceBuilderSetDefaults(devB); nvnDeviceBuilderSetFlags(devB, 0);
  if (!nvnDeviceInitialize(dev, devB)) { printf("NOT_RUN: device initialize failed: %s\n", LastVkError().c_str()); return 3; }
  printf("device: %s (Vulkan %u.%u.%u)  validation=%s subgroupSize=%u\n", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.props.apiVersion),
         VK_API_VERSION_MINOR(g_vk.props.apiVersion), VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.messenger ? "on" : "off", g_vk.p11.subgroupSize);
  if (wantValidation && !sweepOnly) CHECK(g_vk.messenger != VK_NULL_HANDLE, "validation layer was requested but is not active (layer path %s)", layerPath.c_str());
  nvnDeviceSetDepthMode(dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);
  nvnDeviceSetWindowOriginMode(dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
  nvnQueueBuilderSetDefaults(qB); nvnQueueBuilderSetDevice(qB, dev);
  CHECK(nvnQueueInitialize(queue, qB), "queue initialize");

  if (!sweepOnly) {
    // ---- memory, targets, descriptor pools ------------------------------------------------------------------------------
    GuestPool data, shaderPool;
    CHECK(MakePool(&data, dev, 8u << 20, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED), "data pool");
    CHECK(MakePool(&shaderPool, dev, 0x10000, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_SHADER_CODE), "shader pool");
    static GuestTex color[8], depth, texColor, texDepth, texMs, texMsArr;
    for (auto& c : color) CHECK(MakeTex(&c, &data, dev, NVN_FORMAT_RGBA8, 64, 64, 0), "color target");
    CHECK(MakeTex(&depth, &data, dev, NVN_FORMAT_DEPTH32F, 64, 64, 0), "depth target");
    CHECK(MakeTex(&texColor, &data, dev, NVN_FORMAT_RGBA8, 4, 4, 0), "sample texture");
    CHECK(MakeTex(&texDepth, &data, dev, NVN_FORMAT_DEPTH32F, 4, 4, 0), "shadow sample texture");
    CHECK(MakeTex(&texMs, &data, dev, NVN_FORMAT_RGBA8, 4, 4, 0, NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 4), "2dms sample texture");
    CHECK(MakeTex(&texMsArr, &data, dev, NVN_FORMAT_RGBA8, 4, 4, 0, NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY, 4, 2), "2dms array sample texture");
    CHECK(texMs.g()->impl && texMs.g()->impl->samples == 4 && texMsArr.g()->impl && texMsArr.g()->impl->samples == 4 && texMsArr.g()->impl->layers == 2, "multisample textures keep their sample count and layers");
    alignas(16) static uint8_t sampB[0x80], sampPlain[0x30 + 0x40], sampCmp[0x30 + 0x40], texPool[0x20 + 0x40], sampPool[0x20 + 0x40];
    nvnSamplerBuilderSetDefaults(sampB); nvnSamplerBuilderSetDevice(sampB, dev);
    nvnSamplerBuilderSetMinMagFilter(sampB, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
    nvnSamplerBuilderSetWrapMode(sampB, NVN_WRAP_MODE_CLAMP, NVN_WRAP_MODE_CLAMP, NVN_WRAP_MODE_CLAMP);
    CHECK(nvnSamplerInitialize(sampPlain, sampB), "sampler");
    nvnSamplerBuilderSetCompare(sampB, NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE, NVN_DEPTH_FUNC_LESS);
    CHECK(nvnSamplerInitialize(sampCmp, sampB), "compare sampler");
    const int kTexColor = 256, kTexDepth = 257, kTexMs = 258, kTexMsArr = 259, kSampPlain = 256, kSampCmp = 257;
    uint64_t tpOff = Bump(&data, 0x8000), spOff = Bump(&data, 0x2000);
    CHECK(nvnTexturePoolInitialize(texPool, data.blob, (int64_t)tpOff, 512), "texture pool");
    CHECK(nvnSamplerPoolInitialize(sampPool, data.blob, (int64_t)spOff, 512), "sampler pool");
    nvnTexturePoolRegisterTexture(texPool, kTexColor, texColor.blob, nullptr);
    nvnTexturePoolRegisterTexture(texPool, kTexDepth, texDepth.blob, nullptr);
    nvnTexturePoolRegisterTexture(texPool, kTexMs, texMs.blob, nullptr);
    nvnTexturePoolRegisterTexture(texPool, kTexMsArr, texMsArr.blob, nullptr);
    nvnSamplerPoolRegisterSampler(sampPool, kSampPlain, sampPlain);
    nvnSamplerPoolRegisterSampler(sampPool, kSampCmp, sampCmp);

    // buffers: a quad, and a 64 KiB uniform buffer bound at every uniform slot of the variant draws
    alignas(16) static uint8_t bB[0x80], vbuf[0x30 + 0x40], ubuf[0x30 + 0x40];
    auto MakeBuf = [&](uint8_t* blob, uint64_t size) {
      uint64_t off = Bump(&data, size, 256);
      nvnBufferBuilderSetDefaults(bB); nvnBufferBuilderSetDevice(bB, dev); nvnBufferBuilderSetStorage(bB, data.blob, (int64_t)off, (int64_t)size);
      bool ok = nvnBufferInitialize(blob, bB); CHECK(ok, "buffer init"); return ok;
    };
    CHECK(MakeBuf(vbuf, 0x1000) && MakeBuf(ubuf, 0x10000), "buffers");
    Vtx* vtx = (Vtx*)nvnBufferMap(vbuf);
    vtx[0] = {-1, 1, 0.5f, 0, 0}; vtx[1] = {-1, -1, 0.5f, 0, 1}; vtx[2] = {1, 1, 0.5f, 1, 0}; vtx[3] = {1, -1, 0.5f, 1, 1};
    nvnBufferFlushMappedRange(vbuf, 0, 0x1000);
    memset(nvnBufferMap(ubuf), 0, 0x10000); nvnBufferFlushMappedRange(ubuf, 0, 0x10000);
    const uint64_t vbAddr = nvnBufferGetAddress(vbuf), ubAddr = nvnBufferGetAddress(ubuf);

    alignas(16) static uint8_t blend[16], colorSt[16], chan[16], ds[16], ms[0x40], poly[16], attrs[2][4], stream[8];
    nvnBlendStateSetDefaults(blend); nvnColorStateSetDefaults(colorSt); nvnChannelMaskStateSetDefaults(chan);
    nvnDepthStencilStateSetDefaults(ds); nvnMultisampleStateSetDefaults(ms); nvnPolygonStateSetDefaults(poly); nvnPolygonStateSetCullFace(poly, NVN_FACE_NONE);
    nvnVertexAttribStateSetDefaults(attrs[0]); nvnVertexAttribStateSetFormat(attrs[0], NVN_FORMAT_RGB32F, 0); nvnVertexAttribStateSetStreamIndex(attrs[0], 0);
    nvnVertexAttribStateSetDefaults(attrs[1]); nvnVertexAttribStateSetFormat(attrs[1], NVN_FORMAT_RG32F, 12); nvnVertexAttribStateSetStreamIndex(attrs[1], 0);
    nvnVertexStreamStateSetDefaults(stream); nvnVertexStreamStateSetStride(stream, sizeof(Vtx));

    alignas(16) static uint8_t cb[0xA0 + 0x40];
    CHECK(nvnCommandBufferInitialize(cb, dev), "command buffer init");
    auto Record = [&](auto&& body) { nvnCommandBufferBeginRecording(cb); body(); return nvnCommandBufferEndRecording(cb); };
    auto SubmitWait = [&](uint64_t h) { nvnQueueSubmitCommands(queue, 1, &h); nvnQueueFinish(queue); };
    auto State = [&]() {
      nvnCommandBufferBindBlendState(cb, blend); nvnCommandBufferBindColorState(cb, colorSt); nvnCommandBufferBindChannelMaskState(cb, chan);
      nvnCommandBufferBindDepthStencilState(cb, ds); nvnCommandBufferBindMultisampleState(cb, ms); nvnCommandBufferBindPolygonState(cb, poly);
    };
    auto Targets = [&](int n) {
      void* rts[8]; for (int i = 0; i < n; i++) rts[i] = color[i].blob;
      nvnCommandBufferSetRenderTargets(cb, n, rts, nullptr, depth.blob, nullptr);
      nvnCommandBufferSetViewport(cb, 0, 0, 64, 64); nvnCommandBufferSetScissor(cb, 0, 0, 64, 64);
    };
    // The sampled textures are cleared once so they hold defined contents (a depth texture is never uploaded from the guest pool).
    {
      float one[4] = {1, 1, 1, 1};
      uint64_t h = Record([&]() {
        void* rt1[1] = {texColor.blob}; nvnCommandBufferSetRenderTargets(cb, 1, rt1, nullptr, nullptr, nullptr);
        nvnCommandBufferSetViewport(cb, 0, 0, 4, 4); nvnCommandBufferSetScissor(cb, 0, 0, 4, 4);
        nvnCommandBufferClearColor(cb, 0, one, NVN_CLEAR_COLOR_MASK_RGBA);
        nvnCommandBufferSetRenderTargets(cb, 0, nullptr, nullptr, texDepth.blob, nullptr);
        nvnCommandBufferClearDepthStencil(cb, 0.5f, true, 0, 0);
      });
      SubmitWait(h);
    }

    // ---- 2. support buffer and program constants end to end -------------------------------------------------------------
    {
      struct Added { const char* tag; int stage; const std::vector<uint32_t>* spv; Manifest m; uint64_t off = 0; alignas(16) uint8_t ctl[0x710]; std::string key; };
      Added a[2];
      a[0].tag = "NVN-CACHE-TEST-VS-quad"; a[0].stage = NVN_SHADER_STAGE_VERTEX; a[0].spv = &vsSpv;
      a[0].m.set = 0; a[0].m.inputs = {{0, 0}, {1, 1}}; a[0].m.outLocs = {0};
      a[1].tag = "NVN-CACHE-TEST-FS-support"; a[1].stage = NVN_SHADER_STAGE_FRAGMENT; a[1].spv = &fsSupportSpv;
      a[1].m.set = 1; a[1].m.supportBinding = 0; a[1].m.supportSize = 1600;
      a[1].m.inputs = {{0, 0}, {8, 8}};
      a[1].m.ubos.push_back({1, 1, -1, 65536, UBO_PROGRAM_CONSTANTS});
      for (auto& s : a) {
        uint8_t code[64]; memset(code, 0, sizeof code); strcpy((char*)code, s.tag);
        s.off = Bump(&shaderPool, sizeof code, 256); memcpy(shaderPool.mem + s.off, code, sizeof code);
        memset(s.ctl, 0, sizeof s.ctl); *(uint32_t*)s.ctl = 0x98761234u; *(uint32_t*)(s.ctl + 0x704) = sizeof code;
        s.key = Sha256Hex(code, sizeof code);
        ShaderCacheAdd(s.key, s.stage, *s.spv, s.m);
      }
      std::vector<uint8_t> consts(20, 0); float c0 = 0.25f, c1 = 0.125f; memcpy(consts.data(), &c0, 4); memcpy(consts.data() + 16, &c1, 4);
      ShaderCacheSetConstants(a[1].key, consts);
      alignas(16) static uint8_t prog[0x100];
      ShaderData d[2] = {{shaderPool.va + a[0].off, a[0].ctl}, {shaderPool.va + a[1].off, a[1].ctl}};
      CHECK(nvnProgramInitialize(prog, dev) && nvnProgramSetShaders(prog, 2, d), "support test program");
      uint64_t h = Record([&]() {
        Targets(1); State();
        float col[4] = {0, 0, 0, 0}; nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA); nvnCommandBufferClearDepthStencil(cb, 1.0f, true, 0, 0);
        nvnCommandBufferBindVertexAttribState(cb, 2, attrs); nvnCommandBufferBindVertexStreamState(cb, 1, stream);
        nvnCommandBufferBindProgram(cb, prog, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
        nvnCommandBufferBindVertexBuffer(cb, 0, vbAddr, 4 * sizeof(Vtx));
        nvnCommandBufferDrawArrays(cb, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4);
      });
      SubmitWait(h);
      std::vector<uint8_t> img; uint32_t w = 0, hh = 0;
      CHECK(ReadbackTexture(color[0].g()->impl, 0, 0, &img, &w, &hh) && img.size() == 64 * 64 * 4, "readback");
      if (img.size() == 64 * 64 * 4) {
        Px p = At(img, 64, 32, 32);
        printf("support/constants pixel: (%d,%d,%d,%d), expected (255,255,191,96)\n", p.r, p.g, p.b, p.a);
        CHECK(Near(p, {255, 255, 191, 96}), "support data or varying values are wrong: got (%d,%d,%d,%d)", p.r, p.g, p.b, p.a);
      }
      nvnProgramFinalize(prog);
    }

    // ---- 3. variants from variants.jsonl, as NVN programs, drawn with validation on --------------------------------------
    std::vector<Variant> all = ReadVariants(cacheDir + "/variants.jsonl");
    printf("variants.jsonl: %zu distinct (vs, fs, gs) combinations\n", all.size());
    std::vector<Variant> picks;
    if (!variantsFile.empty()) {
      picks = ReadVariants(variantsFile);
      CHECK(!picks.empty(), "no valid variant pairs in %s", variantsFile.c_str());
      printf("targeted variants: %zu\n", picks.size());
    } else {
      std::vector<size_t> gsIdx, plainIdx;
      for (size_t i = 0; i < all.size(); i++) (all[i].gs.empty() ? plainIdx : gsIdx).push_back(i);
      size_t wantGs = std::min<size_t>(gsIdx.size(), (size_t)minGs);
      size_t wantPlain = (size_t)std::max(0, nVariants - (int)wantGs);
      for (size_t k = 0; k < wantGs; k++) picks.push_back(all[gsIdx[k * gsIdx.size() / wantGs]]);
      for (size_t k = 0; k < std::min(wantPlain, plainIdx.size()); k++) picks.push_back(all[plainIdx[k * plainIdx.size() / std::min(wantPlain, plainIdx.size())]]);
    }
    // The cache's 2dms and 2dms_array fragment programs (cache v1 has 5 and 3, by the first 12 hex digits of the stage key) are always drawn: their
    // bindings must parse as multisample, and the draws exercise the multisample dummies, a real multisample texture (even ordinals) and a
    // single-sample texture bound to a multisample binding (odd ordinals, which the replay must refuse and replace with the dummy).
    std::map<std::string, int> msOrdinal;
    {
      static const char* kMsFs[] = {"3bed69cc9538", "43094543406a", "5db5e878fff0", "68d5e0b3aa2d", "ad89da88c92d", "b8fa73d443d8", "cae7746810b6", "f6ef73e109cd"};
      // Full keys from the index. A program no variant uses gets a vertex stage from the cache that writes every location it reads.
      std::vector<std::string> msKeys;
      for (auto& k : okKeys) for (auto* pre : kMsFs) if (k.compare(0, 12, pre) == 0) msKeys.push_back(k);
      std::vector<std::string> vsKeys; { std::set<std::string> seenVs; for (auto& v : all) if (seenVs.insert(v.vs).second) vsKeys.push_back(v.vs); }
      for (auto& fsKey : msKeys) {
        Variant pick; bool found = false;
        for (auto& v : all) if (v.fs == fsKey) { pick = v; found = true; break; }
        if (!found) {
          StageProgram* fsp = ShaderCacheGet(fsKey);
          if (!fsp->ok) continue;
          for (size_t i = 0; i < vsKeys.size() && i < 3000 && !found; i++) {
            StageProgram* vsp = ShaderCacheGet(vsKeys[i]);
            bool covers = vsp->ok;
            if (covers) for (auto& in : fsp->man.inputs) if (std::find(vsp->man.outLocs.begin(), vsp->man.outLocs.end(), in.location) == vsp->man.outLocs.end()) { covers = false; break; }
            if (covers) { pick.vs = vsKeys[i]; pick.fs = fsKey; pick.shader = "multisample test"; found = true; }
            else ShaderCacheDrop(vsKeys[i]);
          }
        }
        if (found) { picks.push_back(pick); int o = (int)msOrdinal.size(); msOrdinal[pick.fs] = o; }
      }
      int n2 = 0, n2a = 0, flagged = 0;
      for (auto& kv : msOrdinal) {
        StageProgram* sp = ShaderCacheGet(kv.first);
        if (!sp->ok) continue;
        for (auto& t : sp->man.texs) { if (t.dim == DIM_2DMS) n2++; if (t.dim == DIM_2DMS_ARRAY) n2a++; if ((t.dim == DIM_2DMS || t.dim == DIM_2DMS_ARRAY) && t.multisample) flagged++; }
      }
      printf("multisample programs: %zu variants, %d 2dms bindings, %d 2dms_array bindings, %d flagged multisample\n", msOrdinal.size(), n2, n2a, flagged);
      CHECK(msOrdinal.size() == 8, "expected the 8 multisample fragment programs of cache v1 in variants.jsonl, found %zu", msOrdinal.size());
      CHECK(n2 >= 1 && n2a >= 1, "the cache's manifests must produce 2dms (%d) and 2dms_array (%d) bindings", n2, n2a);
      CHECK(flagged == n2 + n2a, "%d of %d multisample bindings lack the multisample flag", n2 + n2a - flagged, n2 + n2a);
    }
    size_t built = 0, notOk = 0, incomplete = 0, drawn = 0, withGs = 0, withTex = 0, withShadow = 0, withSupport = 0, withConsts = 0, multiRt = 0;
    std::map<std::string, int> rejected;
    Stats s0 = GetStats();
    const int kBatch = 8;
    alignas(16) static uint8_t progs[kBatch][0x100];
    alignas(16) static uint8_t ctls[kBatch][3][0x710];
    for (size_t base = 0; base < picks.size(); base += kBatch) {
      size_t n = std::min<size_t>(kBatch, picks.size() - base);
      struct Pick { bool live = false; int nColor = 1; int prim = NVN_DRAW_PRIMITIVE_TRIANGLES; std::vector<StageProgram*> sps; };
      Pick pk[kBatch];
      for (size_t i = 0; i < n; i++) {
        const Variant& v = picks[base + i];
        const std::string keys[3] = {v.vs, v.fs, v.gs};
        ShaderData d[3]; int nd = 0; bool okAll = true;
        for (int s = 0; s < 3; s++) {
          if (keys[s].empty()) continue;
          StageProgram* sp = ShaderCacheGet(keys[s]);
          if (sp->ok) ShaderCacheCheckDevice(sp);
          if (!sp->ok) { okAll = false; rejected[sp->reason.substr(0, 90)]++; }
          pk[i].sps.push_back(sp);
          std::string bytes = "NVN-CACHE-TEST-ALIAS-" + keys[s];
          uint64_t off = (uint64_t)((i * 3 + s) * 256);
          memcpy(shaderPool.mem + off, bytes.data(), bytes.size());
          memset(ctls[i][s], 0, sizeof ctls[i][s]); *(uint32_t*)ctls[i][s] = 0x98761234u; *(uint32_t*)(ctls[i][s] + 0x704) = (uint32_t)bytes.size();
          ShaderCacheAlias(Sha256Hex(bytes.data(), bytes.size()), keys[s]);
          d[nd++] = {shaderPool.va + off, ctls[i][s]};
        }
        memset(progs[i], 0, sizeof progs[i]);
        nvnProgramInitialize(progs[i], dev);
        bool complete = nvnProgramSetShaders(progs[i], nd, d);
        if (!okAll) notOk++; else if (!complete) incomplete++;
        pk[i].live = okAll && complete;
        if (!pk[i].live) continue;
        for (StageProgram* sp : pk[i].sps) {
          if (sp->man.stage == NVN_SHADER_STAGE_GEOMETRY) { withGs++; pk[i].prim = GsDrawPrimitive(sp->spv); if (pk[i].prim < 0) pk[i].prim = NVN_DRAW_PRIMITIVE_TRIANGLES; }
          if (sp->man.stage == NVN_SHADER_STAGE_FRAGMENT) {
            uint32_t hi = 0; for (int b = 0; b < 8; b++) if (sp->man.rtMask & (1u << b)) hi = b + 1;
            pk[i].nColor = (int)std::max(1u, hi); if (pk[i].nColor > 1) multiRt++;
          }
          if (!sp->man.texs.empty()) { withTex++; for (auto& t : sp->man.texs) if (t.shadow) { withShadow++; break; } }
          if (sp->man.supportBinding >= 0) withSupport++;
          if (!sp->man.constFile.empty()) withConsts++;
        }
      }
      uint64_t h = Record([&]() {
        State();
        nvnCommandBufferSetTexturePool(cb, texPool); nvnCommandBufferSetSamplerPool(cb, sampPool);
        for (size_t i = 0; i < n; i++) {
          if (!pk[i].live) continue;
          Targets(pk[i].nColor);
          float col[4] = {0, 0, 0, 0}; nvnCommandBufferClearColor(cb, 0, col, NVN_CLEAR_COLOR_MASK_RGBA);
          nvnCommandBufferBindProgram(cb, progs[i], NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
          for (int st = 0; st < 5; st++) for (int u = 0; u < 14; u++) nvnCommandBufferBindUniformBuffer(cb, st, u, ubAddr, 0x10000);
          for (StageProgram* sp : pk[i].sps) {
            // Every texture slot of the stage is bound each draw (the command buffer state carries over between draws). 2D textures get
            // a real color or depth texture (shadow) and its sampler; other dimensions get texture id 0, which is not registered
            // and so reads the replay's dummy image of that dimension.
            for (auto& t : sp->man.texs) {
              if (!t.hasSampler || t.hCbuf != 2 || t.sCbuf != 2 || t.hOff < 0x168 || t.sOff < 0x568) continue;
              bool real = t.dim == 1 && t.res != RES_SAMPLER;
              int realId = t.shadow ? kTexDepth : kTexColor;
              if (t.dim == DIM_2DMS || t.dim == DIM_2DMS_ARRAY) {
                auto mo = msOrdinal.find(sp->key);
                real = t.res != RES_SAMPLER; realId = (mo != msOrdinal.end() && (mo->second & 1)) ? kTexColor : t.dim == DIM_2DMS ? kTexMs : kTexMsArr;
              }
              for (uint32_t k = 0; k < std::max(1u, t.arrayLen); k++) {
                int ti = (int)((t.hOff - 0x168) / 8 + k), si = (int)((t.sOff - 0x568) / 8 + k);
                if (ti > 63 || si > 63) break;
                nvnCommandBufferBindSeparateTexture(cb, sp->man.stage, ti, nvnDeviceGetSeparateTextureHandle(dev, real ? realId : 0));
                nvnCommandBufferBindSeparateSampler(cb, sp->man.stage, si, nvnDeviceGetSeparateSamplerHandle(dev, t.shadow ? kSampCmp : kSampPlain));
              }
            }
          }
          nvnCommandBufferDrawArrays(cb, pk[i].prim, 0, 6);
          drawn++;
        }
      });
      SubmitWait(h);
      for (size_t i = 0; i < n; i++) nvnProgramFinalize(progs[i]);
      built += n;
    }
    Stats s1 = GetStats();
    printf("variants: %zu sampled (%zu with a geometry stage), %zu skipped (a stage failed the device check or did not translate), %zu incomplete\n", built, withGs, notOk, incomplete);
    for (auto& kv : rejected) printf("  skipped %d x  %s\n", kv.second, kv.first.c_str());
    printf("variants: %zu draws recorded, replay executed %llu draws and skipped %llu, built %llu pipelines, %llu pipeline failures\n", drawn,
           (unsigned long long)(s1.draws - s0.draws), (unsigned long long)(s1.skippedDraws - s0.skippedDraws), (unsigned long long)(s1.pipelines - s0.pipelines),
           (unsigned long long)(s1.pipelineFails - s0.pipelineFails));
    printf("variants: %zu stage programs use textures (%zu shadow), %zu have a support buffer, %zu have c[1] constants files, %zu write more than one target\n", withTex, withShadow, withSupport, withConsts, multiRt);
    CHECK(s1.pipelineFails == s0.pipelineFails, "%llu pipelines failed to build", (unsigned long long)(s1.pipelineFails - s0.pipelineFails));
    CHECK(s1.pipelines > s0.pipelines, "no pipeline was built");
    CHECK(s1.draws - s0.draws + s1.skippedDraws - s0.skippedDraws == drawn, "draws executed + skipped (%llu) differ from draws recorded (%zu)", (unsigned long long)(s1.draws - s0.draws + s1.skippedDraws - s0.skippedDraws), drawn);

    nvnQueueFinish(queue);
    nvnCommandBufferFinalize(cb);
    nvnBufferFinalize(vbuf); nvnBufferFinalize(ubuf);
    nvnTexturePoolFinalize(texPool); nvnSamplerPoolFinalize(sampPool); nvnSamplerFinalize(sampPlain); nvnSamplerFinalize(sampCmp);
    for (auto& c : color) nvnTextureFinalize(c.blob);
    nvnTextureFinalize(depth.blob); nvnTextureFinalize(texColor.blob); nvnTextureFinalize(texDepth.blob); nvnTextureFinalize(texMs.blob); nvnTextureFinalize(texMsArr.blob);
    nvnMemoryPoolFinalize(data.blob); nvnMemoryPoolFinalize(shaderPool.blob);
  }

  // ---- 4. sweep: every stage program through the device check, module and descriptor set layout ------------------------------
  if (doSweep) {
    size_t ready = 0, rejectedDev = 0, notLoaded = 0, bad = 0;
    std::map<std::string, int> why;
    std::map<int, size_t> readyByStage;
    std::map<std::string, std::vector<uint32_t>> vsOuts, fsIns;       // interface locations, for the pairing check below
    double tLoad = 0, tCheck = 0, tReady = 0;
    auto Now = []() { return (double)GetTickCount64() / 1000.0; };
    double sweepStart = Now();
    for (size_t i = 0; i < okKeys.size(); i++) {
      if (i && i % 4000 == 0) { double rs, ps; ShaderCacheLoadTimes(&rs, &ps); printf("sweep progress: %zu of %zu stage programs after %.0f s (load %.0f s of which file reads %.0f s and manifest parse %.0f s, device check %.0f s, module and layout %.0f s)\n", i, okKeys.size(), Now() - sweepStart, tLoad, rs, ps, tCheck, tReady); }
      double t0 = Now();
      StageProgram* sp = ShaderCacheGet(okKeys[i]);
      tLoad += Now() - t0;
      if (sp->ok && sp->man.stage == NVN_SHADER_STAGE_VERTEX) vsOuts[okKeys[i]] = sp->man.outLocs;
      if (sp->ok && sp->man.stage == NVN_SHADER_STAGE_FRAGMENT) { auto& v = fsIns[okKeys[i]]; for (auto& in : sp->man.inputs) v.push_back(in.location); }
      if (!sp->ok) { notLoaded++; why["load: " + sp->reason.substr(0, 80)]++; ShaderCacheDrop(okKeys[i]); continue; }
      double t1 = Now();
      if (!ShaderCacheCheckDevice(sp)) { rejectedDev++; why[sp->reason.substr(0, 90)]++; ShaderCacheDrop(okKeys[i]); continue; }
      double t2 = Now(); tCheck += t2 - t1;
      if (ShaderStageReady(sp)) { ready++; readyByStage[sp->man.stage]++; } else { bad++; why["module/layout creation failed"]++; }
      tReady += Now() - t2;
      ShaderCacheDrop(okKeys[i]);
    }
    printf("sweep time: %.1f s (load %.1f s, device check %.1f s, module and layout %.1f s)\n", Now() - sweepStart, tLoad, tCheck, tReady);
    printf("sweep: %zu stage programs: %zu ready (module and set layout built), %zu rejected by the device check, %zu not loaded, %zu module or layout failures\n",
           okKeys.size(), ready, rejectedDev, notLoaded, bad);
    for (auto& kv : readyByStage) printf("  ready %-8s %zu\n", StageName(kv.first), kv.second);
    for (auto& kv : why) printf("  %d x  %s\n", kv.second, kv.first.c_str());
    CHECK(notLoaded == 0 && bad == 0, "%zu stage programs did not load and %zu failed module or layout creation", notLoaded, bad);
    CHECK(ready + rejectedDev == okKeys.size(), "sweep accounting");

    // Pairing: a fragment shader input at a location its vertex shader never writes is undefined in Vulkan (the layer reports it at
    // pipeline creation). Count the variants where that happens, from the manifests of the whole cache.
    {
      std::vector<Variant> every = ReadVariants(cacheDir + "/variants.jsonl");
      size_t mismatch = 0, checked = 0, missingLocs = 0; std::string example;
      std::map<size_t, size_t> hist;
      for (auto& v : every) {
        auto vi = vsOuts.find(v.vs); auto fi = fsIns.find(v.fs);
        if (vi == vsOuts.end() || fi == fsIns.end()) continue;
        checked++;
        size_t miss = 0;
        for (uint32_t loc : fi->second) if (std::find(vi->second.begin(), vi->second.end(), loc) == vi->second.end()) miss++;
        if (miss) { mismatch++; missingLocs += miss; hist[miss]++; if (example.empty()) example = v.shader + " vs " + v.vs.substr(0, 12) + " fs " + v.fs.substr(0, 12); }
      }
      printf("pairing: %zu of %zu distinct (vs, fs) pairs have fragment inputs the vertex shader does not write (%zu locations in all)", mismatch, checked, missingLocs);
      if (!example.empty()) printf("; first: %s", example.c_str());
      printf("\n");
      for (auto& kv : hist) printf("  %zu pairs miss %zu location(s)\n", kv.second, kv.first);
    }
  }

  nvnQueueFinish(queue);
  nvnQueueFinalize(queue);
  nvnDeviceFinalize(dev);

  int realErrors = 0;
  {
    // Known false positives of the validation layer, reported but not counted as failures.
    //  - maxCombinedClipAndCullDistances-00710: the layer adds the ClipDistance and CullDistance sizes of a geometry shader's gl_in and
    //    gl_out blocks together (7 + 7 and 1 + 1 = 16); each block is ClipDistance[7] + CullDistance[1] = 8, the device limit.
    static const std::map<std::string, const char*> benign = {
        {"VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710", "layer sums the gl_in and gl_out blocks of a geometry shader (each is 7 + 1 = 8)"}};
    std::lock_guard<std::mutex> lk(g_vk.valLock);
    int warns = 0, benignErrors = 0;
    for (auto& kv : g_vk.valById) {
      if (!kv.second.error) { warns += kv.second.count; continue; }
      if (benign.count(kv.first)) benignErrors += kv.second.count; else realErrors += kv.second.count;
    }
    printf("validation: %d errors (%d known layer false positives), %d warnings; by message id:\n", realErrors + benignErrors, benignErrors, warns);
    for (auto& kv : g_vk.valById) {
      std::string first = kv.second.first; if (first.size() > 700) first.resize(700);
      auto b = benign.find(kv.first);
      printf("  %s %-58s x%d%s%s\n      first: %s\n", kv.second.error ? (b != benign.end() ? "known" : "ERROR") : "warn ", kv.first.c_str(), kv.second.count,
             b != benign.end() ? "  # " : "", b != benign.end() ? b->second : "", first.c_str());
    }
  }
  CHECK(realErrors == 0, "validation layer reported %d errors", realErrors);
  printf("nvn_cache: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
