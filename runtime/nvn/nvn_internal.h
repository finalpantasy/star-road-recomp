// Star Road Recomp: NVN on Vulkan, internal types (task-12). Our own code (D-008).
//
// Guest visible objects are byte blobs the game allocates (sizes below). Objects that own GPU resources keep
// `{magic, impl*}` at offset 0. Builders and state objects are plain data stored in place.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nvn_enums.h"
#include "nvn_vk.h"

namespace nvn {

// ---- logging ------------------------------------------------------------------------------------------------
void Log(const char* fmt, ...);
void Err(const char* fmt, ...);
void LogOnce(const char* key, const char* fmt, ...);    // once per distinct key
extern int g_logLevel;                                   // 0 errors only, 1 default, 2 verbose

// ---- guest object sizes (public NVN ABI) ---------------------------------------------------------------------
enum : size_t {
  kSizeDevice = 0x3000, kSizeDeviceBuilder = 0x40, kSizeQueue = 0x2000, kSizeQueueBuilder = 0x40, kSizeWindow = 0x180,
  kSizeTexture = 0x80, kSizeTextureView = 0x10, kSizeCommandBuffer = 0xA0, kSizeSync = 0x40, kSizeProgram = 0xC0,
  kSizeMemoryPool = 0x100, kSizeTexturePool = 0x20, kSizeSamplerPool = 0x20, kSizeBlend = 8, kSizeVertexAttrib = 4,
  kSizeVertexStream = 8, kSizeMemoryPoolBuilder = 0x40, kSizeSamplerBuilder = 0x60, kSizeSampler = 0x30,
  kSizeBufferBuilder = 0x40, kSizeBuffer = 0x30, kSizeEvent = 0x40, kSizeEventBuilder = 0x20, kSizeWindowBuilder = 0x40,
  kSizeTextureBuilder = 0x80, kSizeMultisample = 0x18, kSizeDepthStencil = 8, kSizeChannelMask = 4, kSizePolygon = 4,
  kSizeColor = 4,
};

constexpr uint32_t kMagicDevice = 0x4e564e44, kMagicQueue = 0x4e564e51, kMagicPool = 0x4e564e50, kMagicBuffer = 0x4e564e42,
                   kMagicTexture = 0x4e564e54, kMagicSampler = 0x4e564e53, kMagicCmd = 0x4e564e43, kMagicSync = 0x4e564e59,
                   kMagicWindow = 0x4e564e57, kMagicProgram = 0x4e564e47, kMagicTexPool = 0x4e564e74,
                   kMagicSampPool = 0x4e564e73, kMagicEvent = 0x4e564e45;

struct ObjHdr { uint32_t magic; uint32_t pad; void* impl; };   // first 16 bytes of every impl backed object

// Builders and in place state (layouts are ours; sizes above are the real ABI sizes and are asserted).
struct DeviceBuilderG { uint32_t flags; };
struct QueueBuilderG { void* dev; uint64_t cmdMem, computeMem, controlMem, flushThreshold; uint32_t flags, priority; };
struct MemPoolBuilderG { void* dev; void* memory; uint64_t size; uint32_t flags; };
struct BufferBuilderG { void* dev; void* pool; uint64_t offset, size; };
struct SamplerBuilderG {
  void* dev; uint32_t minFilter, magFilter, wrapS, wrapT, wrapR, compareMode, compareFunc, reduction;
  float lodBias, minLod, maxLod, maxAniso; float border[4];
};
struct TextureBuilderG {
  void* dev; void* pool; uint64_t offset; uint32_t target, format, width, height, depth, levels, samples, flags;
  uint32_t swz[4]; uint32_t dsMode; uint64_t stride; uint8_t packaged[8]; void* storageMem; uint32_t pad2;
};
struct WindowBuilderG { void* dev; void* native; uint32_t numActive, presentInterval, numTextures; void* textures[4]; };

// ---- NVN state objects (in place, tightly packed to the real sizes) ------------------------------------------
struct BlendStateG { uint8_t target, srcRGB, dstRGB, srcA, dstA, eq, advMode, flags; };   // eq: rgb | a << 3 | overlap << 6
struct ColorStateG { uint8_t blendMask, logicOp, alphaFunc, pad; };
struct ChannelMaskG { uint32_t masks; };                                                  // 4 bits per target
struct PolygonStateG { uint8_t cull, front, mode, offsets; };
struct DepthStencilG { uint8_t flags /*b0 test b1 write b2 stencil*/, depthFunc, func /*front|back<<4*/, pad,
                       opF /*fail|zfail<<4*/, opF2 /*zpass*/, opB, opB2; };
struct MultisampleG { uint8_t samples, enable, a2c, a2cDither, grid, rasterSamples, pad[2]; uint8_t rest[0x10]; };
struct VertexAttribG { uint16_t offset; uint8_t format, stream; };
struct VertexStreamG { uint32_t stride, divisor; };
static_assert(sizeof(BlendStateG) <= kSizeBlend && sizeof(ColorStateG) <= kSizeColor && sizeof(ChannelMaskG) <= kSizeChannelMask &&
              sizeof(PolygonStateG) <= kSizePolygon && sizeof(DepthStencilG) <= kSizeDepthStencil && sizeof(MultisampleG) <= kSizeMultisample &&
              sizeof(VertexAttribG) <= kSizeVertexAttrib && sizeof(VertexStreamG) <= kSizeVertexStream, "state sizes");

// NVNtexture (0x80 bytes) in place; NVNtextureView (0x10 bytes).
struct TextureView {          // 16 bytes
  uint8_t flags;              // b0 target b1 format b2 swizzle b3 levels b4 layers
  uint8_t target, format, pad;
  uint8_t swz[4];             // NVN swizzle values 0..5
  uint16_t minLevel, numLevels;
  uint16_t minLayer, numLayers;
};
static_assert(sizeof(TextureView) == 16, "NVNtextureView is 16 bytes");

struct TextureImpl;
struct TextureG {             // 0x80 bytes: header then the descriptive fields the getters return
  uint32_t magic, pad; TextureImpl* impl;
  uint32_t target, format, width, height, depth, levels, samples, flags, dsMode;
  uint32_t swz[4];
  uint64_t stride, storageSize, memOffset, zcullSize;
  void* pool; uint64_t textureAddr; uint64_t debug;
};
static_assert(sizeof(TextureG) <= kSizeTexture, "NVNtexture is 0x80 bytes");

// ---- impl objects ----------------------------------------------------------------------------------------------
struct Device;
struct Queue;
struct Pool;
struct Buf;

struct Device {
  uint32_t flags = 0;
  uint32_t depthMode = NVN_DEPTH_MODE_NEAR_IS_ZERO;
  uint32_t originMode = NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
  uint64_t t0 = 0;
  uint64_t debugId = 1;
};

// Virtual GPU address space: every pool gets a base; address = base + offset. 0x40 bits, nothing real.
constexpr uint64_t kVaBase = 0x0000'0040'0000'0000ull;

struct Pool {
  uint32_t flags = 0;
  uint8_t* memory = nullptr;          // guest (== host) pointer, 4 KiB aligned
  uint64_t size = 0;
  bool ownsMemory = false;
  uint64_t va = 0;                    // virtual base
  VkBuffer buffer = VK_NULL_HANDLE;   // whole pool as a buffer (host import) or mirrored
  MemAlloc mem;
  bool imported = false;
  bool mirrored = false;              // no host import: device local copy synced at submit
  Buf* mirrorStage = nullptr;
  uint64_t gen = 1;
  std::string label;
  std::vector<TextureImpl*> textures;   // textures stored in this pool (re-upload on FlushMappedRange)
};

struct BufImpl {
  Pool* pool = nullptr; void* guestPool = nullptr;
  uint64_t offset = 0, size = 0;
  uint64_t debugId = 0;
};

struct SamplerImpl {
  VkSampler sampler = VK_NULL_HANDLE;
  SamplerBuilderG desc{};
};

struct TextureImpl {
  VkImage image = VK_NULL_HANDLE;
  MemAlloc mem;
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  uint32_t layers = 1, levels = 1, width = 1, height = 1, depth = 1, samples = 1;
  bool isCube = false, is3D = false;
  bool uploaded = false;              // swizzled data copied from the guest pool
  bool gpuWritten = false;            // used as a render target or copy destination first: never upload over it
  bool renderTarget = false;
  bool presentable = false;
  uint8_t rawFormatInfo = 0;
  uint32_t nvnFormat = 0;
  int sr_fmt = 0, sr_srgb = 0;
  uint64_t lastUse = 0;               // timeline value of the last submission touching it
  std::map<uint64_t, VkImageView> views;   // by view key
  Pool* pool = nullptr;
  uint64_t poolOffset = 0;
  bool owned = true;                  // false: swapchain image or external
  uint64_t uploadGen = 0;
  uint32_t uploadedPoolGen = 0;
  bool layoutInit = false;            // moved out of UNDEFINED into GENERAL
  uint64_t storageSize = 0;
};

struct Program;
struct Sync { uint64_t value = 0; bool signaled = false; uint64_t eventId = 0; };

struct WindowImpl;
struct QueueImpl;

// ---- shader cache ----------------------------------------------------------------------------------------------
struct ManifestUbo { uint32_t binding; int cbuf; int nvnIndex; uint32_t size; };
struct ManifestTex { uint32_t binding; int dim; bool shadow; uint32_t arrayLen; int hCbuf; uint32_t hOff; bool hasSampler; int sCbuf; uint32_t sOff;
                     int kind; int index; int samplerIndex; };
struct ManifestImg { uint32_t binding; int dim; int hCbuf; uint32_t hOff; int index; };
struct ManifestSsbo { uint32_t binding; uint32_t descOff; int nvnIndex; };
struct ManifestInput { uint32_t location; uint32_t attr; };
struct Manifest {
  std::string key; int stage = 0; uint32_t set = 0;
  std::vector<ManifestUbo> ubos; std::vector<ManifestTex> texs; std::vector<ManifestImg> imgs; std::vector<ManifestSsbo> ssbos;
  std::vector<ManifestInput> inputs; std::vector<uint32_t> outLocs;
  uint32_t rtMask = 1; uint32_t localSize[3] = {1, 1, 1}; bool subgroup32 = false, needsLayerOrVp = false;
};
struct StageProgram {
  std::string key; Manifest man; std::vector<uint32_t> spv; VkShaderModule module = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE; bool ok = false; std::string reason;
};
struct ProgramImpl {
  std::vector<StageProgram*> stages;   // indexed by order given
  StageProgram* byStage[6] = {};
  bool complete = false; bool isCompute = false; uint64_t id = 0;
};

// shader cache API (host and runtime)
bool ShaderCacheLoad(const std::string& root);                            // index.jsonl loader
void ShaderCacheAdd(const std::string& key, int stage, const std::vector<uint32_t>& spv, const Manifest& m);   // tests, replace/
StageProgram* ShaderCacheGet(const std::string& key);
void ShaderCacheReleaseVk();                                              // device teardown
void ReplayShutdown();                                                    // device teardown: pipelines, layouts, dummies, scratch
std::string Sha256Hex(const void* data, size_t n);
const char* StageName(int stage);

// ---- command stream --------------------------------------------------------------------------------------------
enum class Op : uint16_t {
  BindBlend, BindColor, BindChannelMask, BindDepthStencil, BindMultisample, BindPolygon, BindVertexAttrib, BindVertexStream,
  BindProgram, BindVertexBuffer, BindVertexBuffers, BindUniform, BindUniforms, BindStorage, BindSepTexture, BindSepSampler, BindImage,
  SetRenderTargets, SetViewport, SetScissor, SetDepthRange, SetDepthBounds, SetDepthClamp, SetLineWidth, SetPointSize, SetPatchSize,
  SetPolyOffset, SetPrimRestart, SetRasterDiscard, SetSampleMask, SetStencilRef, SetStencilMask, SetStencilValueMask,
  SetTexturePool, SetSamplerPool, Draw, DrawInstanced, DrawElements, DrawElementsInstanced, DrawIndirect, DrawElementsIndirect,
  Dispatch, DispatchIndirect, ClearColor, ClearDepthStencil, CopyBufToBuf, CopyBufToTex, CopyTexToBuf, CopyTexToTex, Barrier,
  FenceSync, UpdateUniform, CallCommands, ReportCounter, TiledCache, Nop, Downsample,
};

struct Cmd {
  Op op = Op::Nop;
  int32_t i[8] = {};
  int64_t l[5] = {};
  float f[4] = {};
  uint64_t p[4] = {};
  uint8_t st[0x18] = {};
  std::vector<uint8_t> blob;
  void* obj[3] = {};
};

struct CmdList {
  std::vector<Cmd> cmds;
  uint64_t id = 0;
  uint32_t holds = 0;
};

// ---- replay state ---------------------------------------------------------------------------------------------
struct TexPool {
  struct Ent { TextureImpl* tex = nullptr; TextureG* texG = nullptr; TextureView view{}; bool hasView = false; bool isImage = false; };
  std::vector<Ent> ents;
  uint32_t size = 0;
};
struct SampPool { std::vector<SamplerImpl*> ents; std::vector<SamplerImpl*> owned; uint32_t size = 0; };

// ---- proc table -----------------------------------------------------------------------------------------------
struct ProcEntry {
  const char* name;
  void* native;                 // typed C function pointer (null for stubs)
  void (*thunk)(void* ctx);     // Ctx adapter (set by the runtime layer), null if unavailable
  const char* sig;              // "ret:args" from nvn_sigs.inc, "?" if unknown
  const char* stubReason;       // null when implemented
  const char* implSig;          // signature derived from the C++ implementation (null for stubs); tests compare it with `sig`
};
size_t ProcCount();
const ProcEntry* ProcAt(size_t i);
const ProcEntry* ProcFind(const char* name);

// ---- shared helpers -------------------------------------------------------------------------------------------
Pool* PoolFromVa(uint64_t va, uint64_t* off);
uint64_t NextVaBase(uint64_t size);
VkFormat VkFormatFromNvn(uint32_t nvnFormat, int* srFmt, int* srgb, VkImageAspectFlags* aspect);
VkFormat VertexFormatFromNvn(uint32_t nvnFormat, bool* isInt);
uint32_t NvnFormatBytes(uint32_t nvnFormat);
VkComponentSwizzle SwizzleFromNvn(uint32_t s, VkComponentSwizzle identity);
bool IsDepthFormat(VkFormat f);

}  // namespace nvn
