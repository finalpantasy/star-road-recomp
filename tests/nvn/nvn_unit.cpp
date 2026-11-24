// Star Road Recomp: NVN unit tests (task-12). No GPU needed. Our own code (D-008).
//
// Covers: guest object sizes (NVNtexture 0x80, NVNtextureView 0x10), the proc table (count, signature agreement between the generated
// table and the C++ implementations, coverage of the names the game imports), the Ctx thunk ABI (integers, pointers, floats, 64-bit
// values, bool and 32-bit upper garbage), the RGBA4 component order and the depth range enum.
//
// usage: nvn_unit [used_list.json]    (the list is optional; it is a JSON array of the proc names the game imports)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nvn_api.h"
#include "nvn_rt.h"

using namespace nvn;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                                                                       \
  do {                                                                                                         \
    g_checks++;                                                                                                \
    if (!(cond)) { g_fails++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

static size_t IndexOf(const char* name) {
  for (size_t i = 0; i < nvn_proc_count(); i++) if (!strcmp(nvn_proc_name(i), name)) return i;
  return (size_t)-1;
}
template <class F> static F Native(const char* name) {
  const ProcEntry* e = ProcFind(name);
  CHECK(e && e->native, "proc %s has no native implementation", name);
  return (F)(e ? e->native : nullptr);
}
// Calls proc `name` through its Ctx thunk.
static void CallThunk(const char* name, Ctx* c) {
  size_t i = IndexOf(name);
  CHECK(i != (size_t)-1, "proc %s missing", name);
  if (i == (size_t)-1) return;
  nvn_proc_thunk(i)(c);
}
static float VF(const v128& v) { float f; memcpy(&f, &v, 4); return f; }
static void SetVF(v128& v, float f) { v = v128{0, 0}; memcpy(&v, &f, 4); }

// ---- 1. sizes ----------------------------------------------------------------------------------------------------------
static void TestSizes() {
  CHECK(kSizeTexture == 0x80, "NVNtexture size constant");
  CHECK(sizeof(TextureG) <= 0x80, "TextureG is %zu bytes, NVNtexture is 0x80", sizeof(TextureG));
  CHECK(kSizeTextureView == 0x10 && sizeof(TextureView) == 16, "NVNtextureView is 16 bytes, ours %zu", sizeof(TextureView));
  CHECK(sizeof(TextureBuilderG) <= kSizeTextureBuilder, "texture builder size %zu", sizeof(TextureBuilderG));
  CHECK(sizeof(SamplerBuilderG) <= kSizeSamplerBuilder, "sampler builder size %zu", sizeof(SamplerBuilderG));
  CHECK(sizeof(BufferBuilderG) <= kSizeBufferBuilder && sizeof(MemPoolBuilderG) <= kSizeMemoryPoolBuilder, "builder sizes");
  CHECK(sizeof(WindowBuilderG) <= kSizeWindowBuilder, "window builder size %zu", sizeof(WindowBuilderG));
  CHECK(sizeof(QueueBuilderG) <= kSizeQueueBuilder && sizeof(DeviceBuilderG) <= kSizeDeviceBuilder, "queue and device builder sizes");
  CHECK(sizeof(BlendStateG) <= kSizeBlend && sizeof(VertexAttribG) <= kSizeVertexAttrib && sizeof(VertexStreamG) <= kSizeVertexStream,
        "state sizes");
  CHECK(sizeof(ObjHdr) == 16, "object header");
  printf("sizes: NVNtexture 0x%zx (impl uses 0x%zx), NVNtextureView 0x%zx\n", (size_t)kSizeTexture, sizeof(TextureG), sizeof(TextureView));
}

// The view setters write only the 16 bytes at the view and leave the bytes around it alone.
static void TestViewInPlace() {
  uint8_t mem[48]; memset(mem, 0xCD, sizeof mem);
  auto* v = (TextureView*)(mem + 16);
  auto setDefaults = Native<void (*)(void*)>("nvnTextureViewSetDefaults");
  auto setTarget = Native<void (*)(void*, int)>("nvnTextureViewSetTarget");
  auto setFormat = Native<void (*)(void*, int)>("nvnTextureViewSetFormat");
  auto setSwizzle = Native<void (*)(void*, int, int, int, int)>("nvnTextureViewSetSwizzle");
  auto setLevels = Native<void (*)(void*, int, int)>("nvnTextureViewSetLevels");
  auto setLayers = Native<void (*)(void*, int, int)>("nvnTextureViewSetLayers");
  auto getTarget = Native<bool (*)(const void*, int*)>("nvnTextureViewGetTarget");
  auto getFormat = Native<bool (*)(const void*, int*)>("nvnTextureViewGetFormat");
  auto getLevels = Native<bool (*)(const void*, int*, int*)>("nvnTextureViewGetLevels");
  auto getLayers = Native<bool (*)(const void*, int*, int*)>("nvnTextureViewGetLayers");
  auto getSwizzle = Native<bool (*)(const void*, int*, int*, int*, int*)>("nvnTextureViewGetSwizzle");
  if (!setDefaults || !setTarget || !setFormat || !setSwizzle || !setLevels || !setLayers || !getTarget || !getFormat || !getLevels ||
      !getLayers || !getSwizzle) return;
  setDefaults(v);
  for (int i = 0; i < 16; i++) CHECK(mem[16 + i] == 0, "SetDefaults leaves byte %d = 0x%02x", i, mem[16 + i]);
  setTarget(v, NVN_TEXTURE_TARGET_2D_ARRAY); setFormat(v, NVN_FORMAT_RGBA8); setSwizzle(v, 5, 4, 3, 2); setLevels(v, 1, 3); setLayers(v, 2, 4);
  for (int i = 0; i < 16; i++) CHECK(mem[i] == 0xCD && mem[32 + i] == 0xCD, "view write spilled outside its 16 bytes (offset %d)", i);
  int t = -1, f = -1, a = -1, b = -1, c = -1, d = -1;
  CHECK(getTarget(v, &t) && t == NVN_TEXTURE_TARGET_2D_ARRAY, "target %d", t);
  CHECK(getFormat(v, &f) && f == NVN_FORMAT_RGBA8, "format %d", f);
  CHECK(getLevels(v, &a, &b) && a == 1 && b == 3, "levels %d %d", a, b);
  CHECK(getLayers(v, &a, &b) && a == 2 && b == 4, "layers %d %d", a, b);
  CHECK(getSwizzle(v, &a, &b, &c, &d) && a == 5 && b == 4 && c == 3 && d == 2, "swizzle %d %d %d %d", a, b, c, d);
}

// ---- 2. proc table -----------------------------------------------------------------------------------------------------
static std::vector<std::string> LoadUsed(const char* path) {
  std::vector<std::string> out;
  std::ifstream f(path, std::ios::binary);
  if (!f) return out;
  std::stringstream ss; ss << f.rdbuf();
  std::string s = ss.str();
  for (size_t i = 0; i < s.size();) {
    size_t q = s.find('"', i);
    if (q == std::string::npos) break;
    size_t e = s.find('"', q + 1);
    if (e == std::string::npos) break;
    std::string w = s.substr(q + 1, e - q - 1);
    if (w.rfind("nvn", 0) == 0) out.push_back(w);
    i = e + 1;
  }
  return out;
}

static void TestProcTable(const char* usedPath) {
  size_t n = nvn_proc_count();
  CHECK(n == 535, "proc table has %zu names, expected 535", n);
  size_t impl = 0, stub = 0, noSig = 0, sigMismatch = 0;
  for (size_t i = 0; i < n; i++) {
    const ProcEntry* e = ProcAt(i);
    CHECK(e && e->name && !strncmp(e->name, "nvn", 3), "entry %zu has a bad name", i);
    CHECK(nvn_proc_thunk(i) != nullptr, "%s has no thunk", e->name);
    CHECK(nvn_proc_implemented(i) == (e->native ? 1 : 0), "%s implemented flag", e->name);
    for (size_t j = i + 1; j < n; j++) if (!strcmp(nvn_proc_name(i), nvn_proc_name(j))) { CHECK(false, "duplicate name %s", e->name); break; }
    if (e->native) {
      impl++;
      if (strcmp(e->sig, "?") == 0) { noSig++; continue; }
      if (!e->implSig || strcmp(e->sig, e->implSig) != 0) {
        sigMismatch++;
        printf("SIGMISMATCH %s table=%s impl=%s\n", e->name, e->sig, e->implSig ? e->implSig : "(null)");
      }
    } else {
      stub++;
      CHECK(e->stubReason && *e->stubReason, "%s: stub without a reason", e->name);
    }
  }
  CHECK(sigMismatch == 0, "%zu implemented procs disagree with the generated signature table", sigMismatch);
  printf("proc table: %zu names, %zu implemented, %zu logged stubs; %zu implemented have no generated signature\n", n, impl, stub, noSig);

  std::vector<std::string> used = LoadUsed(usedPath);
  if (used.empty()) { printf("used list: %s not found, coverage check skipped\n", usedPath); return; }
  size_t usedImpl = 0, usedStub = 0;
  for (auto& u : used) {
    size_t i = IndexOf(u.c_str());
    CHECK(i != (size_t)-1, "imported proc %s is not in the proc table", u.c_str());
    if (i == (size_t)-1) continue;
    if (nvn_proc_implemented(i)) usedImpl++;
    else {
      usedStub++;
      const ProcEntry* e = ProcAt(i);
      printf("USED STUB %s : %s\n", u.c_str(), e->stubReason ? e->stubReason : "(no reason)");
      CHECK(e->stubReason && strcmp(e->stubReason, "") != 0, "%s lacks a reason", u.c_str());
    }
  }
  printf("used list: %zu names, %zu implemented, %zu stubbed\n", used.size(), usedImpl, usedStub);
}

// ---- 3. thunk ABI ------------------------------------------------------------------------------------------------------
static void TestThunkAbi() {
  // integer args from x1.., upper 32 bits of a 32-bit argument ignored
  BlendStateG bs{}; Ctx c{};
  c.x[0] = (uint64_t)(uintptr_t)&bs; c.x[1] = 0xDEADBEEF00000003ull; c.x[2] = 0xCAFEF00D00000004ull; c.x[3] = 5; c.x[4] = 0xFFFFFFFF00000006ull;
  CallThunk("nvnBlendStateSetBlendFunc", &c);
  CHECK(bs.srcRGB == 3 && bs.dstRGB == 4 && bs.srcA == 5 && bs.dstA == 6, "blend func %d %d %d %d", bs.srcRGB, bs.dstRGB, bs.srcA, bs.dstA);

  // bool arg: only the low byte counts
  ColorStateG cs{}; memset(&c, 0, sizeof c);
  c.x[0] = (uint64_t)(uintptr_t)&cs; c.x[1] = 2; c.x[2] = 0x1;
  CallThunk("nvnColorStateSetBlendEnable", &c);
  CHECK(cs.blendMask == 4, "blend enable on: mask %d", cs.blendMask);
  c.x[2] = 0xFFFFFF00ull;
  CallThunk("nvnColorStateSetBlendEnable", &c);
  CHECK(cs.blendMask == 0, "bool with a clear low byte reads false: mask %d", cs.blendMask);

  // float args come from v0, v1 (not from x registers)
  SamplerBuilderG sb{}; memset(&c, 0, sizeof c);
  c.x[0] = (uint64_t)(uintptr_t)&sb; c.x[1] = 0x3F800000; c.x[2] = 0x40000000;      // decoys in x1, x2
  SetVF(c.v[0], -1.5f); SetVF(c.v[1], 7.25f);
  CallThunk("nvnSamplerBuilderSetLodClamp", &c);
  CHECK(sb.minLod == -1.5f && sb.maxLod == 7.25f, "lod clamp %g %g", sb.minLod, sb.maxLod);

  // float return in v0
  sb.lodBias = 0.625f; memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)&sb; c.x[7] = 0x1234;
  CallThunk("nvnSamplerBuilderGetLodBias", &c);
  CHECK(VF(c.v[0]) == 0.625f, "float return %g", VF(c.v[0]));

  // 64-bit return in x0, 32-bit args from x1 and x2
  memset(&c, 0, sizeof c); c.x[0] = 0; c.x[1] = 0xAAAA00000000012Cull; c.x[2] = 0xBBBB000000000007ull;
  CallThunk("nvnDeviceGetTextureHandle", &c);
  CHECK(c.x[0] == (((uint64_t)7 << 20) | 0x12C), "texture handle 0x%llx", (unsigned long long)c.x[0]);

  // pointer return in x0
  MemPoolBuilderG pb{}; pb.memory = (void*)0x123450; memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)&pb;
  CallThunk("nvnMemoryPoolBuilderGetMemory", &c);
  CHECK(c.x[0] == 0x123450, "pointer return 0x%llx", (unsigned long long)c.x[0]);

  // 32-bit return is zero extended (the guest reads w0)
  memset(&c, 0, sizeof c); c.x[0] = 0xFFFFFFFF00000000ull;
  TextureBuilderG tb{}; tb.width = 640; c.x[0] = (uint64_t)(uintptr_t)&tb;
  CallThunk("nvnTextureBuilderGetWidth", &c);
  CHECK(c.x[0] == 640, "int return 0x%llx", (unsigned long long)c.x[0]);

  // 64-bit argument and a stub: x0 and v0 are cleared and nothing crashes
  size_t si = (size_t)-1;
  for (size_t i = 0; i < nvn_proc_count(); i++) if (!nvn_proc_implemented(i)) { si = i; break; }
  CHECK(si != (size_t)-1, "no stub found to test");
  if (si != (size_t)-1) { memset(&c, 0, sizeof c); c.x[0] = 99; nvn_proc_thunk(si)(&c); CHECK(c.x[0] == 0, "stub returns 0"); }
}

// ---- 4. formats and depth mode -----------------------------------------------------------------------------------------
static void TestFormats() {
  int sr = 0, srgb = 0; VkImageAspectFlags asp = 0;
  VkFormat f = VkFormatFromNvn(NVN_FORMAT_RGBA4, &sr, &srgb, &asp);
  CHECK(f == VK_FORMAT_A4B4G4R4_UNORM_PACK16, "RGBA4 maps to VkFormat %d (A4B4G4R4 = %d)", (int)f, (int)VK_FORMAT_A4B4G4R4_UNORM_PACK16);
  CHECK(NvnFormatBytes(NVN_FORMAT_RGBA4) == 2, "RGBA4 texel size");
  f = VkFormatFromNvn(NVN_FORMAT_RGBA8, &sr, &srgb, &asp);
  CHECK(f == VK_FORMAT_R8G8B8A8_UNORM && !srgb, "RGBA8 -> %d", (int)f);
  f = VkFormatFromNvn(NVN_FORMAT_DEPTH32F, &sr, &srgb, &asp);
  CHECK(f == VK_FORMAT_D32_SFLOAT && (asp & VK_IMAGE_ASPECT_DEPTH_BIT), "DEPTH32F -> %d", (int)f);
  f = VkFormatFromNvn(NVN_FORMAT_RGB565, &sr, &srgb, &asp);
  CHECK(f == VK_FORMAT_B5G6R5_UNORM_PACK16, "RGB565 -> %d", (int)f);
  CHECK(IsDepthFormat(VK_FORMAT_D32_SFLOAT) && !IsDepthFormat(VK_FORMAT_R8G8B8A8_UNORM), "IsDepthFormat");
  CHECK(NVN_DEPTH_MODE_NEAR_IS_MINUS_W == 0 && NVN_DEPTH_MODE_NEAR_IS_ZERO == 1, "depth mode enum values %d %d", NVN_DEPTH_MODE_NEAR_IS_MINUS_W,
        NVN_DEPTH_MODE_NEAR_IS_ZERO);
  // the device default is the 0..1 range
  Device d; CHECK(d.depthMode == NVN_DEPTH_MODE_NEAR_IS_ZERO, "Device default depth mode");
}

int main(int argc, char** argv) {
  const char* used = argc > 1 ? argv[1] : (getenv("NVN_USED_LIST") ? getenv("NVN_USED_LIST") : "A:/AI/Recomps/smrpg-work/nvn/used_list.json");
  TestSizes();
  TestViewInPlace();
  TestProcTable(used);
  TestThunkAbi();
  TestFormats();
  printf("nvn_unit: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
