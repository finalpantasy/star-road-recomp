// Star Road Recomp: present-time upscaling, FSR 1 (task-23). Our own code. The two compute shaders (fsr_pass.comp) call AMD FidelityFX FSR 1
// (MIT), fetched by tools/thirdparty/fetch_fsr.py and compiled by upscale.cmake; without the FSR files this builds with the pass disabled and
// Record() always returns false (the plain blit).
//
// The pass:  game image (raw UNORM view, GENERAL)  --EASU-->  RGBA16F  --RCAS-->  RGBA8 (BGRA byte order when the swapchain is BGRA)
//            --raw copy (same size) or blit (target differs from the swapchain)-->  swapchain image.
// FSR 1 works on the encoded values the game wrote, so an sRGB game target is read through a UNORM view and written back as the same bytes.
#include "upscale.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "../../nvn/nvn_internal.h"

#if defined(SMRPG_UPSCALE_FSR)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wignored-qualifiers"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#pragma clang diagnostic pop
static const uint32_t kEasuSpv[] =
#include "fsr_easu.inc"
;
static const uint32_t kRcasSpv[] =
#include "fsr_rcas.inc"
;
#endif

namespace upscale {
namespace {

using namespace nvn;

struct Img {
  VkImage img = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; MemAlloc mem;
  uint32_t w = 0, h = 0; VkFormat fmt = VK_FORMAT_UNDEFINED; bool init = false;
};

struct PushC { uint32_t c0[4], c1[4], c2[4], c3[4], flags[4]; };

std::atomic<int> g_mode{-1};                     // -1: not read yet
std::atomic<float> g_sharp{0.25f};
std::atomic<uint32_t> g_tw{0}, g_th{0};
std::string g_err;
bool g_envRead = false;

// ---- extra Vulkan entry points (not in nvn_vk.h) -----------------------------------------------------------------------
PFN_vkCreateQueryPool pCreateQueryPool; PFN_vkDestroyQueryPool pDestroyQueryPool; PFN_vkCmdResetQueryPool pCmdResetQueryPool;
PFN_vkGetQueryPoolResults pGetQueryPoolResults; PFN_vkCmdWriteTimestamp2 pCmdWriteTimestamp2;

struct State {
  bool ready = false, failed = false, haveTimestamps = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline easu = VK_NULL_HANDLE, rcas = VK_NULL_HANDLE;
  VkSampler smp = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE; VkQueryPool qp = VK_NULL_HANDLE;
  Img easuImg, outImg, outTyped;                 // outTyped: RGBA8 sRGB twin of outImg for the scaled blit onto an sRGB swapchain
  std::map<VkImage, std::pair<VkImageView, uint64_t>> srcViews;   // game image -> raw UNORM view (+ format/size key)
  // GPU time samples (the queries of the previous frame are read at the start of the next Record)
  bool qPending = false, qCopyUsed = false; double sumEasu = 0, sumRcas = 0, sumCopy = 0, maxTotal = 0; uint32_t nSamples = 0; uint32_t lastW = 0, lastH = 0, lastSrcW = 0, lastSrcH = 0;
} S;

// ---- capture and present hash (debug, env driven; off costs one flag test) -----------------------------------------------
struct Cap {
  bool read = false;
  std::string dir; std::vector<uint64_t> frames; uint64_t every = 0;          // SMRPG_UPSCALE_CAPTURE_DIR / _FRAMES / _EVERY: PNG pair (game image, pass output)
  bool hash = false; std::vector<uint64_t> hashFrames; uint64_t hashEvery = 0;   // SMRPG_PRESENT_HASH=frames or "every:N": hash of the presented image
  std::string hashDir;                                                          // SMRPG_PRESENT_HASH_DIR: also write the swapchain image as PNG
} C;

struct Pending {
  bool active = false; uint64_t frame = 0; bool wantPng = false;
  Buf srcBuf, outBuf, swapBuf, refBuf;
  uint32_t srcW = 0, srcH = 0, outW = 0, outH = 0, swW = 0, swH = 0; VkFormat srcFmt = VK_FORMAT_UNDEFINED, outFmt = VK_FORMAT_UNDEFINED, swFmt = VK_FORMAT_UNDEFINED;
  bool hasSrc = false, hasOut = false, hasSwap = false, hasRef = false; Mode mode = Mode::Off;
} P;
Img g_refImg;                                                                    // scratch image for the reference blit (hash debug)

std::vector<uint64_t> ParseList(const char* s) {
  std::vector<uint64_t> v;
  while (s && *s) { char* e; unsigned long long n = strtoull(s, &e, 10); if (e == s) break; v.push_back(n); if (*e != ',') break; s = e + 1; }
  return v;
}

void ReadEnv() {
  if (g_envRead) return;
  g_envRead = true;
  const char* m = getenv("SMRPG_UPSCALE");
  int mode = 0;
  if (m && (!_stricmp(m, "fsr1"))) mode = 1;
  else if (m && !_stricmp(m, "fsr2")) mode = 2;
  else if (m && *m && _stricmp(m, "off")) Log("upscale: unknown SMRPG_UPSCALE '%s', using off", m);
  int expect = -1; g_mode.compare_exchange_strong(expect, mode);
  if (const char* s = getenv("SMRPG_UPSCALE_SHARPNESS")) g_sharp = (float)atof(s);
  if (const char* t = getenv("SMRPG_UPSCALE_TARGET")) { unsigned w = 0, h = 0; if (sscanf(t, "%ux%u", &w, &h) == 2) { g_tw = w; g_th = h; } }
  if (const char* d = getenv("SMRPG_UPSCALE_CAPTURE_DIR")) {
    C.dir = d;
    const char* f = getenv("SMRPG_UPSCALE_CAPTURE_FRAMES"); C.frames = ParseList(f ? f : "");
    if (const char* e = getenv("SMRPG_UPSCALE_CAPTURE_EVERY")) C.every = strtoull(e, nullptr, 10);
  }
  if (const char* h = getenv("SMRPG_PRESENT_HASH")) {
    C.hash = true;
    if (!strncmp(h, "every:", 6)) C.hashEvery = strtoull(h + 6, nullptr, 10); else C.hashFrames = ParseList(h);
    if (const char* d = getenv("SMRPG_PRESENT_HASH_DIR")) C.hashDir = d;
  }
  C.read = true;
}

// ---- PNG (stored deflate blocks: big but needs no zlib) ---------------------------------------------------------------
uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
  static uint32_t t[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; t[i] = x; } init = true; }
  c = ~c; for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8); return ~c;
}
void PutBE(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void Chunk(FILE* f, const char* type, const std::vector<uint8_t>& d) {
  std::vector<uint8_t> hdr; PutBE(hdr, (uint32_t)d.size()); fwrite(hdr.data(), 1, 4, f);
  std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), d.begin(), d.end());
  fwrite(td.data(), 1, td.size(), f); std::vector<uint8_t> crc; PutBE(crc, Crc32(td.data(), td.size())); fwrite(crc.data(), 1, 4, f);
}
bool WritePng(const std::string& path, const uint8_t* rgb, uint32_t w, uint32_t h) {
  FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr; PutBE(ihdr, w); PutBE(ihdr, h); ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0); Chunk(f, "IHDR", ihdr);
  size_t row = (size_t)w * 3 + 1, raw = row * h;
  std::vector<uint8_t> z; z.push_back(0x78); z.push_back(0x01);
  uint32_t a = 1, b = 0; size_t pos = 0;
  std::vector<uint8_t> line(row);
  // build the zlib stream in 65535 byte stored blocks over the filtered rows
  std::vector<uint8_t> all; all.reserve(raw);
  for (uint32_t y = 0; y < h; y++) { all.push_back(0); all.insert(all.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3); }
  for (size_t i = 0; i < all.size(); i++) { a = (a + all[i]) % 65521; b = (b + a) % 65521; }
  while (pos < all.size()) {
    size_t n = std::min<size_t>(65535, all.size() - pos); bool last = pos + n == all.size();
    z.push_back(last ? 1 : 0); z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), all.begin() + (ptrdiff_t)pos, all.begin() + (ptrdiff_t)(pos + n)); pos += n;
  }
  PutBE(z, (b << 16) | a);
  Chunk(f, "IDAT", z); Chunk(f, "IEND", {}); fclose(f); return true;
}

// 8-bit 4 channel texels to RGB (bgra: bytes are B,G,R,A)
void ToRgb(const uint8_t* px, uint32_t w, uint32_t h, bool bgra, std::vector<uint8_t>* out) {
  out->resize((size_t)w * h * 3);
  for (size_t i = 0, n = (size_t)w * h; i < n; i++) { const uint8_t* p = px + i * 4; uint8_t* o = out->data() + i * 3; o[0] = bgra ? p[2] : p[0]; o[1] = p[1]; o[2] = bgra ? p[0] : p[2]; }
}
bool IsBgra(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB; }
bool IsRgba8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB; }
bool IsSrgb8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }
uint64_t Fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

// ---- Vulkan helpers ------------------------------------------------------------------------------------------------------
void DestroyImg(Img& I) {
  auto& F = g_vk.f;
  if (I.view) F.vkDestroyImageView(g_vk.dev, I.view, nullptr);
  if (I.img) F.vkDestroyImage(g_vk.dev, I.img, nullptr);
  if (I.mem.mem) FreeMemory(I.mem);
  I = Img{};
}
bool MakeImg(Img& I, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, bool withView) {
  auto& F = g_vk.f;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (F.vkCreateImage(g_vk.dev, &ci, nullptr, &I.img) != VK_SUCCESS) { I.img = VK_NULL_HANDLE; return false; }
  VkMemoryRequirements rq; F.vkGetImageMemoryRequirements(g_vk.dev, I.img, &rq);
  if (!AllocMemory(MemKind::Device, rq, &I.mem)) { F.vkDestroyImage(g_vk.dev, I.img, nullptr); I.img = VK_NULL_HANDLE; return false; }
  F.vkBindImageMemory(g_vk.dev, I.img, I.mem.mem, I.mem.offset);
  I.w = w; I.h = h; I.fmt = fmt; I.init = false;
  if (withView) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = I.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (F.vkCreateImageView(g_vk.dev, &vi, nullptr, &I.view) != VK_SUCCESS) return false;
  }
  return true;
}
void Barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2}; mb.srcStageMask = src; mb.srcAccessMask = sa; mb.dstStageMask = dst; mb.dstAccessMask = da;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb; g_vk.f.vkCmdPipelineBarrier2(cb, &di);
}
void ToGeneral(VkCommandBuffer cb, Img& I) {
  if (I.init) return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = I.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b; g_vk.f.vkCmdPipelineBarrier2(cb, &di);
  I.init = true;
}

void FreeAll() {
  auto& F = g_vk.f;
  if (!g_vk.inited) return;
  F.vkDeviceWaitIdle(g_vk.dev);
  DestroyImg(S.easuImg); DestroyImg(S.outImg); DestroyImg(S.outTyped); DestroyImg(g_refImg);
  for (auto& kv : S.srcViews) F.vkDestroyImageView(g_vk.dev, kv.second.first, nullptr);
  S.srcViews.clear();
  if (S.easu) F.vkDestroyPipeline(g_vk.dev, S.easu, nullptr);
  if (S.rcas) F.vkDestroyPipeline(g_vk.dev, S.rcas, nullptr);
  if (S.pl) F.vkDestroyPipelineLayout(g_vk.dev, S.pl, nullptr);
  if (S.dsl) F.vkDestroyDescriptorSetLayout(g_vk.dev, S.dsl, nullptr);
  if (S.pool) F.vkDestroyDescriptorPool(g_vk.dev, S.pool, nullptr);
  if (S.smp) F.vkDestroySampler(g_vk.dev, S.smp, nullptr);
  if (S.qp && pDestroyQueryPool) pDestroyQueryPool(g_vk.dev, S.qp, nullptr);
  for (Buf* b : {&P.srcBuf, &P.outBuf, &P.swapBuf, &P.refBuf}) if (b->buf) DestroyBuf(b);
  P.active = false;
  S = State{};
}

#if defined(SMRPG_UPSCALE_FSR)
bool Init() {
  auto& F = g_vk.f;
  auto ld = [&](const char* n) { return F.vkGetDeviceProcAddr(g_vk.dev, n); };
  pCreateQueryPool = (PFN_vkCreateQueryPool)ld("vkCreateQueryPool"); pDestroyQueryPool = (PFN_vkDestroyQueryPool)ld("vkDestroyQueryPool");
  pCmdResetQueryPool = (PFN_vkCmdResetQueryPool)ld("vkCmdResetQueryPool"); pGetQueryPoolResults = (PFN_vkGetQueryPoolResults)ld("vkGetQueryPoolResults");
  pCmdWriteTimestamp2 = (PFN_vkCmdWriteTimestamp2)ld("vkCmdWriteTimestamp2");
  VkDescriptorSetLayoutBinding bd[3]{};
  bd[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bd[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bd[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 3; dl.pBindings = bd;
  if (F.vkCreateDescriptorSetLayout(g_vk.dev, &dl, nullptr, &S.dsl) != VK_SUCCESS) return false;
  VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushC)};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &S.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
  if (F.vkCreatePipelineLayout(g_vk.dev, &pli, nullptr, &S.pl) != VK_SUCCESS) return false;
  auto mk = [&](const uint32_t* code, size_t bytes, VkPipeline* out) {
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize = bytes; sm.pCode = code; VkShaderModule m;
    if (F.vkCreateShaderModule(g_vk.dev, &sm, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pi.layout = S.pl;
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    VkResult r = F.vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, nullptr, out);
    F.vkDestroyShaderModule(g_vk.dev, m, nullptr); return r == VK_SUCCESS;
  };
  if (!mk(kEasuSpv, sizeof(kEasuSpv), &S.easu) || !mk(kRcasSpv, sizeof(kRcasSpv), &S.rcas)) return false;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = 0.25f;
  if (F.vkCreateSampler(g_vk.dev, &si, nullptr, &S.smp) != VK_SUCCESS) return false;
  VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_SAMPLER, 2}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 2; dp.poolSizeCount = 3; dp.pPoolSizes = ps;
  if (F.vkCreateDescriptorPool(g_vk.dev, &dp, nullptr, &S.pool) != VK_SUCCESS) return false;
  S.haveTimestamps = g_vk.props.limits.timestampComputeAndGraphics && pCreateQueryPool && pCmdWriteTimestamp2 && pGetQueryPoolResults && pCmdResetQueryPool;
  if (S.haveTimestamps) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 4;
    if (pCreateQueryPool(g_vk.dev, &qi, nullptr, &S.qp) != VK_SUCCESS) S.haveTimestamps = false;
  }
  return true;
}

// Raw UNORM view of the game's final image (an sRGB target is read as its encoded bytes). null: format not supported by the pass.
VkImageView SrcView(VkImage img, VkFormat fmt, uint32_t w, uint32_t h) {
  VkFormat vf = fmt == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : fmt == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : fmt;
  if (vf != VK_FORMAT_R8G8B8A8_UNORM && vf != VK_FORMAT_B8G8R8A8_UNORM) return VK_NULL_HANDLE;
  uint64_t key = ((uint64_t)fmt << 40) ^ ((uint64_t)w << 20) ^ h;
  auto it = S.srcViews.find(img);
  if (it != S.srcViews.end()) { if (it->second.second == key) return it->second.first; g_vk.f.vkDestroyImageView(g_vk.dev, it->second.first, nullptr); S.srcViews.erase(it); }
  if (S.srcViews.size() > 8) { for (auto& kv : S.srcViews) g_vk.f.vkDestroyImageView(g_vk.dev, kv.second.first, nullptr); S.srcViews.clear(); }   // the caller waited for the previous frame
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = vf; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView v; if (g_vk.f.vkCreateImageView(g_vk.dev, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
  S.srcViews[img] = {v, key}; return v;
}
#endif

// Reads the previous frame's finished work: timestamps and captures
void Harvest() {
  if (S.qPending && S.qp) {
    uint64_t t[4] = {};
    if (pGetQueryPoolResults(g_vk.dev, S.qp, 0, 4, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      double ms = g_vk.props.limits.timestampPeriod * 1e-6;
      double e = (double)(t[1] - t[0]) * ms, r = (double)(t[2] - t[1]) * ms, c = S.qCopyUsed ? (double)(t[3] - t[2]) * ms : 0;
      S.sumEasu += e; S.sumRcas += r; S.sumCopy += c; S.maxTotal = std::max(S.maxTotal, e + r + c); S.nSamples++;
      if (S.nSamples == 120) {
        Log("upscale: gpu time over %u frames, %ux%u -> %ux%u: easu %.3f ms, rcas %.3f ms, copy/blit %.3f ms, total %.3f ms (worst %.3f ms)", S.nSamples, S.lastSrcW, S.lastSrcH, S.lastW, S.lastH,
            S.sumEasu / S.nSamples, S.sumRcas / S.nSamples, S.sumCopy / S.nSamples, (S.sumEasu + S.sumRcas + S.sumCopy) / S.nSamples, S.maxTotal);
        S.sumEasu = S.sumRcas = S.sumCopy = S.maxTotal = 0; S.nSamples = 0;
      }
    }
    S.qPending = false;
  }
  if (P.active) {
    char path[1024]; std::vector<uint8_t> rgb;
    if (P.hasSrc) {
      if (P.wantPng) { ToRgb((const uint8_t*)P.srcBuf.map, P.srcW, P.srcH, IsBgra(P.srcFmt), &rgb); snprintf(path, sizeof path, "%s\\upscale_%06llu_in.png", C.dir.c_str(), (unsigned long long)P.frame); WritePng(path, rgb.data(), P.srcW, P.srcH); }
    }
    if (P.hasOut && P.wantPng) {
      ToRgb((const uint8_t*)P.outBuf.map, P.outW, P.outH, false, &rgb);
      snprintf(path, sizeof path, "%s\\upscale_%06llu_out.png", C.dir.c_str(), (unsigned long long)P.frame); WritePng(path, rgb.data(), P.outW, P.outH);
      Log("upscale: captured frame %llu: %ux%u in, %ux%u out (%s)", (unsigned long long)P.frame, P.srcW, P.srcH, P.outW, P.outH, path);
    }
    if (P.hasSwap) {
      size_t n = (size_t)P.swW * P.swH * 4; uint64_t h = Fnv((const uint8_t*)P.swapBuf.map, n);
      const char* cmp = "";
      if (P.hasRef) cmp = memcmp(P.swapBuf.map, P.refBuf.map, n) == 0 ? " ref-blit=equal" : " ref-blit=DIFFERENT";
      Log("present-hash: frame %llu mode %s %ux%u fmt %d fnv1a64 %016llx%s", (unsigned long long)P.frame, P.mode == Mode::Off ? "off" : P.mode == Mode::Fsr1 ? "fsr1" : "fsr2", P.swW, P.swH, (int)P.swFmt, (unsigned long long)h, cmp);
      if (!C.hashDir.empty()) { ToRgb((const uint8_t*)P.swapBuf.map, P.swW, P.swH, IsBgra(P.swFmt), &rgb); snprintf(path, sizeof path, "%s\\present_%06llu.png", C.hashDir.c_str(), (unsigned long long)P.frame); WritePng(path, rgb.data(), P.swW, P.swH); }
    }
    for (Buf* b : {&P.srcBuf, &P.outBuf, &P.swapBuf, &P.refBuf}) if (b->buf) DestroyBuf(b);
    P = Pending{};
  }
}

bool InList(const std::vector<uint64_t>& v, uint64_t f) { return std::find(v.begin(), v.end(), f) != v.end(); }

}  // namespace

// ---- public API -----------------------------------------------------------------------------------------------------------
Mode CurrentMode() { ReadEnv(); return (Mode)g_mode.load(); }
void SetMode(Mode m) { ReadEnv(); g_mode = (int)m; }
void SetSharpness(float s) { g_sharp = s; }
void SetTarget(uint32_t w, uint32_t h) { g_tw = w; g_th = h; }
const char* LastError() { return g_err.c_str(); }
void Shutdown() { FreeAll(); }

bool WantSwapchainReadback() { ReadEnv(); return C.hash; }

bool Record(const PresentIO& io) {
  ReadEnv();
  Harvest();
  Mode mode = CurrentMode();
  if (mode == Mode::Off) return false;
#if !defined(SMRPG_UPSCALE_FSR)
  if (g_err.empty()) { g_err = "built without FSR (run tools/thirdparty/fetch_fsr.py and rebuild)"; Log("upscale: %s; presenting with the plain blit", g_err.c_str()); }
  return false;
#else
  if (mode == Mode::Fsr2) {
    if (g_err.empty()) { g_err = "fsr2 needs per-pixel motion vectors, depth and camera jitter; the game's render graph does not expose them at present time (docs/PLATFORM.md)"; Log("upscale: %s; presenting with the plain blit", g_err.c_str()); }
    return false;
  }
  if (S.failed) return false;
  auto& F = g_vk.f;
  if (!S.ready) {
    if (!Init()) { S.failed = true; g_err = "pass setup failed"; Log("upscale: %s", g_err.c_str()); FreeAll(); S.failed = true; return false; }
    S.ready = true;
    Log("upscale: FSR 1 ready (gpu timestamps %s)", S.haveTimestamps ? "on" : "unavailable");
  }
  VkImageView sv = SrcView(io.src, io.srcFormat, io.srcW, io.srcH);
  if (!sv) { if (g_err.empty()) { g_err = "game image format not supported by the pass"; Log("upscale: %s (vkFormat %d); presenting with the plain blit", g_err.c_str(), (int)io.srcFormat); } return false; }
  if (!IsBgra(io.dstFormat) && !IsRgba8(io.dstFormat)) { if (g_err.empty()) { g_err = "swapchain format not supported by the pass"; Log("upscale: %s", g_err.c_str()); } return false; }
  uint32_t ow = g_tw ? g_tw.load() : io.dstW, oh = g_th ? g_th.load() : io.dstH;
  ow = std::min(std::max(ow, 16u), 16384u); oh = std::min(std::max(oh, 16u), 16384u);
  const bool direct = (ow == io.dstW && oh == io.dstH);
  const bool dstSrgb = IsSrgb8(io.dstFormat);
  if (S.outImg.w != ow || S.outImg.h != oh) {
    F.vkDeviceWaitIdle(g_vk.dev);
    DestroyImg(S.easuImg); DestroyImg(S.outImg); DestroyImg(S.outTyped);
    bool ok = MakeImg(S.easuImg, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, true) &&
              MakeImg(S.outImg, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true);
    if (ok && !direct && dstSrgb) ok = MakeImg(S.outTyped, VK_FORMAT_R8G8B8A8_SRGB, ow, oh, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
    if (!ok) { S.failed = true; g_err = "could not make the pass images"; Log("upscale: %s at %ux%u", g_err.c_str(), ow, oh); DestroyImg(S.easuImg); DestroyImg(S.outImg); DestroyImg(S.outTyped); return false; }
    Log("upscale: FSR 1 target %ux%u (game image %ux%u, swapchain %ux%u, %s)", ow, oh, io.srcW, io.srcH, io.dstW, io.dstH, direct ? "direct copy" : "scaled blit");
  } else if (!direct && dstSrgb && !S.outTyped.img) {
    if (!MakeImg(S.outTyped, VK_FORMAT_R8G8B8A8_SRGB, ow, oh, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false)) { S.failed = true; return false; }
  }
  VkCommandBuffer cb = io.cb;
  const bool capNow = !C.dir.empty() && (InList(C.frames, io.frame) || (C.every && io.frame % C.every == 0)) && !P.active;
  // constants
  PushC pe{}, pr{};
  AU1 c0[4], c1[4], c2[4], c3[4];
  FsrEasuCon(c0, c1, c2, c3, (AF1)io.srcW, (AF1)io.srcH, (AF1)io.srcW, (AF1)io.srcH, (AF1)ow, (AF1)oh);
  memcpy(pe.c0, c0, 16); memcpy(pe.c1, c1, 16); memcpy(pe.c2, c2, 16); memcpy(pe.c3, c3, 16);
  const float sharp = g_sharp.load(); const bool rcasOn = sharp >= 0.0f;
  AU1 rc[4]; FsrRcasCon(rc, rcasOn ? sharp : 0.0f); memcpy(pr.c0, rc, 16);
  const bool swapRB = direct && IsBgra(io.dstFormat);                                  // the raw copy keeps bytes, so write them in the swapchain's order
  const uint32_t flipBit = io.flipY ? 4u : 0u;
  pe.flags[0] = flipBit; pe.flags[1] = ow; pe.flags[2] = oh;
  pr.flags[0] = (swapRB ? 1u : 0u) | (rcasOn ? 2u : 0u); pr.flags[1] = ow; pr.flags[2] = oh;
  // descriptors (the previous frame is finished: QueuePresent waits on its timeline value)
  F.vkResetDescriptorPool(g_vk.dev, S.pool, 0);
  VkDescriptorSet ds[2]; VkDescriptorSetLayout ls[2] = {S.dsl, S.dsl};
  VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool = S.pool; da.descriptorSetCount = 2; da.pSetLayouts = ls;
  if (F.vkAllocateDescriptorSets(g_vk.dev, &da, ds) != VK_SUCCESS) return false;
  VkDescriptorImageInfo in0{VK_NULL_HANDLE, sv, VK_IMAGE_LAYOUT_GENERAL}, out0{VK_NULL_HANDLE, S.easuImg.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo in1{VK_NULL_HANDLE, S.easuImg.view, VK_IMAGE_LAYOUT_GENERAL}, out1{VK_NULL_HANDLE, S.outImg.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo sm{S.smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet w[6]{};
  auto wr = [&](int i, VkDescriptorSet d, uint32_t b, VkDescriptorType t, const VkDescriptorImageInfo* ii) { w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[i].dstSet = d; w[i].dstBinding = b; w[i].descriptorCount = 1; w[i].descriptorType = t; w[i].pImageInfo = ii; };
  wr(0, ds[0], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &in0); wr(1, ds[0], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &out0); wr(2, ds[0], 2, VK_DESCRIPTOR_TYPE_SAMPLER, &sm);
  wr(3, ds[1], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &in1); wr(4, ds[1], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &out1); wr(5, ds[1], 2, VK_DESCRIPTOR_TYPE_SAMPLER, &sm);
  F.vkUpdateDescriptorSets(g_vk.dev, 6, w, 0, nullptr);
  // record
  ToGeneral(cb, S.easuImg); ToGeneral(cb, S.outImg);
  const bool ts = S.haveTimestamps;
  if (ts) { pCmdResetQueryPool(cb, S.qp, 0, 4); pCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, S.qp, 0); }
  const uint32_t gx = (ow + 15) / 16, gy = (oh + 15) / 16;
  F.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.easu);
  F.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl, 0, 1, &ds[0], 0, nullptr);
  F.vkCmdPushConstants(cb, S.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushC), &pe);
  F.vkCmdDispatch(cb, gx, gy, 1);
  if (ts) pCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, S.qp, 1);
  Barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  F.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.rcas);
  F.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl, 0, 1, &ds[1], 0, nullptr);
  F.vkCmdPushConstants(cb, S.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushC), &pr);
  F.vkCmdDispatch(cb, gx, gy, 1);
  if (ts) pCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, S.qp, 2);
  Barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  if (direct) {
    VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.dstSubresource = cp.srcSubresource; cp.extent = {ow, oh, 1};
    F.vkCmdCopyImage(cb, S.outImg.img, VK_IMAGE_LAYOUT_GENERAL, io.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  } else {
    VkImage from = S.outImg.img;
    if (dstSrgb) {                                                                   // the blit encodes into an sRGB target: give it an sRGB source so the bytes stay as they are
      VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.dstSubresource = cp.srcSubresource; cp.extent = {ow, oh, 1};
      VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; ib.oldLayout = S.outTyped.init ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.image = S.outTyped.img; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &ib; F.vkCmdPipelineBarrier2(cb, &di); S.outTyped.init = true;
      F.vkCmdCopyImage(cb, S.outImg.img, VK_IMAGE_LAYOUT_GENERAL, S.outTyped.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
      Barrier(cb, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      from = S.outTyped.img;
    }
    VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1] = {(int32_t)ow, (int32_t)oh, 1}; bl.dstOffsets[1] = {(int32_t)io.dstW, (int32_t)io.dstH, 1};
    F.vkCmdBlitImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, io.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
  }
  if (ts) { pCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, S.qp, 3); S.qPending = true; S.qCopyUsed = true; }
  S.lastW = ow; S.lastH = oh; S.lastSrcW = io.srcW; S.lastSrcH = io.srcH;
  if (capNow) {                                                                      // PNG pair: the game image as the game made it, and the pass output
    VkDeviceSize ssz = (VkDeviceSize)io.srcW * io.srcH * 4, osz = (VkDeviceSize)ow * oh * 4;
    if (CreateBuf(&P.srcBuf, ssz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemKind::HostVisible) && CreateBuf(&P.outBuf, osz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemKind::HostVisible)) {
      Barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      VkBufferImageCopy c1_{}; c1_.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c1_.imageExtent = {io.srcW, io.srcH, 1};
      F.vkCmdCopyImageToBuffer(cb, io.src, VK_IMAGE_LAYOUT_GENERAL, P.srcBuf.buf, 1, &c1_);
      VkBufferImageCopy c2_{}; c2_.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c2_.imageExtent = {ow, oh, 1};
      F.vkCmdCopyImageToBuffer(cb, S.outImg.img, VK_IMAGE_LAYOUT_GENERAL, P.outBuf.buf, 1, &c2_);
      Barrier(cb, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
      P.active = true; P.wantPng = true; P.frame = io.frame; P.srcW = io.srcW; P.srcH = io.srcH; P.outW = ow; P.outH = oh; P.srcFmt = io.srcFormat; P.hasSrc = P.hasOut = true; P.mode = mode;
    } else { for (Buf* b : {&P.srcBuf, &P.outBuf}) if (b->buf) DestroyBuf(b); }
  }
  return true;
#endif
}

// After the overlay: hash (and optionally dump) the image that is about to be presented. Only with SMRPG_PRESENT_HASH set, and then the swapchain has
// TRANSFER_SRC usage (WantSwapchainReadback). With upscale off it also redoes today's plain blit into a scratch image and compares the two byte for byte.
void DebugAfterOverlay(const PresentIO& io) {
  if (!C.hash || P.active) return;
  if (!(InList(C.hashFrames, io.frame) || (C.hashEvery && io.frame % C.hashEvery == 0))) return;
  auto& F = g_vk.f; VkCommandBuffer cb = io.cb;
  VkDeviceSize sz = (VkDeviceSize)io.dstW * io.dstH * 4;
  if (!CreateBuf(&P.swapBuf, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemKind::HostVisible)) return;
  bool ref = CurrentMode() == Mode::Off && CreateBuf(&P.refBuf, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemKind::HostVisible);
  if (ref && (g_refImg.w != io.dstW || g_refImg.h != io.dstH || g_refImg.fmt != io.dstFormat)) {
    F.vkDeviceWaitIdle(g_vk.dev); DestroyImg(g_refImg);
    if (!MakeImg(g_refImg, io.dstFormat, io.dstW, io.dstH, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false)) ref = false;
  }
  auto trans = [&](VkImage im, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b; F.vkCmdPipelineBarrier2(cb, &di);
  };
  VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {io.dstW, io.dstH, 1};
  if (ref) {   // today's blit, instruction for instruction (same filter, flip and extents) into the scratch image
    trans(g_refImg.img, g_refImg.init ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); g_refImg.init = true;
    VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstSubresource = bl.srcSubresource; bl.srcOffsets[1] = {(int32_t)io.srcW, (int32_t)io.srcH, 1};
    if (io.flipY) { bl.srcOffsets[0].y = (int32_t)io.srcH; bl.srcOffsets[1].y = 0; } bl.dstOffsets[1] = {(int32_t)io.dstW, (int32_t)io.dstH, 1};
    F.vkCmdBlitImage(cb, io.src, VK_IMAGE_LAYOUT_GENERAL, g_refImg.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    trans(g_refImg.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    F.vkCmdCopyImageToBuffer(cb, g_refImg.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.refBuf.buf, 1, &bc);
  }
  trans(io.dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  F.vkCmdCopyImageToBuffer(cb, io.dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.swapBuf.buf, 1, &bc);
  trans(io.dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  Barrier(cb, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  P.active = true; P.frame = io.frame; P.swW = io.dstW; P.swH = io.dstH; P.swFmt = io.dstFormat; P.hasSwap = true; P.hasRef = ref; P.mode = CurrentMode();
}

}  // namespace upscale
