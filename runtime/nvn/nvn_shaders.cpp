// Star Road Recomp: NVN shader cache reader (task-12), consumer of docs/SHADER_CACHE.md v1. Our own code (D-008).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "nvn_internal.h"

namespace nvn {

// ---- SHA-256 ------------------------------------------------------------------------------------------------
namespace {
struct Sha256 {
  uint32_t h[8]; uint64_t len = 0; uint8_t buf[64]; size_t n = 0;
  static uint32_t R(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
  Sha256() { static const uint32_t i[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; memcpy(h, i, 32); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
      0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
      0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
      0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
      0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = R(e, 6) ^ R(e, 11) ^ R(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
      uint32_t S0 = R(a, 2) ^ R(a, 13) ^ R(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t* p, size_t k) {
    len += k;
    while (k) {
      size_t t = std::min(k, 64 - n);
      memcpy(buf + n, p, t); n += t; p += t; k -= t;
      if (n == 64) { block(buf); n = 0; }
    }
  }
  std::string finish() {
    uint64_t bits = len * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (n != 56) update(&z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(l, 8);
    char out[65];
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
  }
};
}  // namespace

std::string Sha256Hex(const void* data, size_t n) {
  Sha256 s; s.update((const uint8_t*)data, n); return s.finish();
}

const char* StageName(int s) {
  static const char* n[] = {"vs", "fs", "gs", "tcs", "tes", "cs"};
  return s >= 0 && s < 6 ? n[s] : "?";
}

// ---- minimal JSON ---------------------------------------------------------------------------------------------
namespace {
struct J {
  enum T { Null, Num, Str, Arr, Obj, Bool } t = Null;
  double n = 0; std::string s; std::vector<J> a; std::vector<std::pair<std::string, J>> o; bool b = false;
  const J* get(const char* k) const { for (auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
  int geti(const char* k, int d = 0) const { auto* v = get(k); return v && v->t == Num ? (int)v->n : d; }
  std::string gets(const char* k) const { auto* v = get(k); return v && v->t == Str ? v->s : ""; }
  bool getb(const char* k) const { auto* v = get(k); return v && v->t == Bool ? v->b : false; }
};
struct JP {
  const char* p; const char* e;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  bool parse(J& j) {
    ws(); if (p >= e) return false;
    char c = *p;
    if (c == '{') {
      p++; j.t = J::Obj; ws();
      if (p < e && *p == '}') { p++; return true; }
      for (;;) { J k; ws(); if (!parse(k) || k.t != J::Str) return false; ws(); if (p >= e || *p++ != ':') return false;
        J v; if (!parse(v)) return false; j.o.emplace_back(k.s, std::move(v)); ws();
        if (p < e && *p == ',') { p++; continue; } if (p < e && *p == '}') { p++; return true; } return false; }
    }
    if (c == '[') {
      p++; j.t = J::Arr; ws();
      if (p < e && *p == ']') { p++; return true; }
      for (;;) { J v; if (!parse(v)) return false; j.a.push_back(std::move(v)); ws();
        if (p < e && *p == ',') { p++; continue; } if (p < e && *p == ']') { p++; return true; } return false; }
    }
    if (c == '"') {
      p++; j.t = J::Str;
      while (p < e && *p != '"') { if (*p == '\\' && p + 1 < e) { p++; char x = *p; j.s += x == 'n' ? '\n' : x == 't' ? '\t' : x; } else j.s += *p; p++; }
      if (p >= e) return false; p++; return true;
    }
    if (!strncmp(p, "true", 4)) { j.t = J::Bool; j.b = true; p += 4; return true; }
    if (!strncmp(p, "false", 5)) { j.t = J::Bool; j.b = false; p += 5; return true; }
    if (!strncmp(p, "null", 4)) { p += 4; return true; }
    char* q; j.t = J::Num; j.n = strtod(p, &q); if (q == p) return false; p = q; return true;
  }
};
bool ParseJson(const std::string& s, J& out) { JP p{s.data(), s.data() + s.size()}; return p.parse(out); }

std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
int DimFromStr(const std::string& d) {
  static const char* n[] = {"1d", "2d", "3d", "cube", "2d_array", "cube_array", "buffer"};
  for (int i = 0; i < 7; i++) if (d == n[i]) return i;
  return 1;
}
int StageFromStr(const std::string& s) {
  for (int i = 0; i < 6; i++) if (s == StageName(i)) return i;
  return 0;
}
void ReadLoc(const J* h, int* cbuf, uint32_t* off) {
  *cbuf = -1; *off = 0;
  if (h && h->t == J::Obj) { *cbuf = h->geti("cbuf", -1); *off = (uint32_t)h->geti("offset", 0); }
}
int ResFromStr(const std::string& s) {
  static const char* n[] = {"", "uniform_buffer", "combined_image_sampler", "sampled_image", "sampler", "uniform_texel_buffer", "storage_image", "storage_texel_buffer", "storage_buffer"};
  for (int i = 1; i < 9; i++) if (s == n[i]) return i;
  return RES_DEFAULT;
}
int SourceFromManifest(const std::string& src, int cbuf, int nvnIndex) {
  if (src == "nvn") return UBO_NVN;
  if (src == "driver_c0") return UBO_DRIVER_C0;
  if (src == "handles_c2") return UBO_HANDLES_C2;
  if (src == "program_constants") return UBO_PROGRAM_CONSTANTS;
  if (nvnIndex >= 0) return UBO_NVN;                       // v1 manifest: classify by constant buffer slot
  return cbuf == 0 ? UBO_DRIVER_C0 : cbuf == 2 ? UBO_HANDLES_C2 : cbuf == 1 ? UBO_PROGRAM_CONSTANTS : UBO_NVN;
}
int TypeFromStr(const std::string& s) { return s == "int" ? 1 : s == "uint" ? 2 : 0; }
bool ParseManifest(const J& j, Manifest& m) {
  m.key = j.gets("key"); m.stage = StageFromStr(j.gets("stage"));
  if (auto* s = j.get("sets")) m.set = (uint32_t)s->geti("set", 0);
  if (auto* a = j.get("ubos")) for (auto& u : a->a) {
    ManifestUbo x{(uint32_t)u.geti("binding"), u.geti("cbuf"), u.geti("nvn_index", -1), (uint32_t)u.geti("size")};
    x.source = SourceFromManifest(u.gets("source"), x.cbuf, x.nvnIndex);
    if (x.source == UBO_PROGRAM_CONSTANTS) m.constFile = u.gets("file");
    m.ubos.push_back(x);
  }
  if (auto* a = j.get("textures")) for (auto& t : a->a) {
    ManifestTex x{}; x.binding = (uint32_t)t.geti("binding"); x.dim = DimFromStr(t.gets("dim")); x.shadow = t.getb("shadow");
    x.arrayLen = (uint32_t)t.geti("array_len", 1); ReadLoc(t.get("handle"), &x.hCbuf, &x.hOff);
    auto* sh = t.get("sampler_handle"); x.hasSampler = sh && sh->t == J::Obj; if (x.hasSampler) ReadLoc(sh, &x.sCbuf, &x.sOff);
    auto* nv = t.get("nvn"); x.kind = 0; x.index = 0; x.samplerIndex = 0;
    if (nv) { std::string k = nv->gets("kind"); x.kind = k == "combined" ? 0 : k == "separate" ? 1 : 2; x.index = nv->geti("index"); x.samplerIndex = nv->geti("sampler_index"); }
    x.res = ResFromStr(t.gets("vk_type")); x.multisample = t.getb("multisample");
    m.texs.push_back(x);
  }
  if (auto* a = j.get("images")) for (auto& t : a->a) {
    ManifestImg x{}; x.binding = (uint32_t)t.geti("binding"); x.dim = DimFromStr(t.gets("dim")); ReadLoc(t.get("handle"), &x.hCbuf, &x.hOff);
    if (auto* nv = t.get("nvn")) x.index = nv->geti("index");
    x.res = ResFromStr(t.gets("vk_type")); m.imgs.push_back(x);
  }
  if (auto* a = j.get("ssbos")) for (auto& t : a->a) {
    ManifestSsbo x{}; x.binding = (uint32_t)t.geti("binding"); x.nvnIndex = t.geti("nvn_index", -1); x.write = t.getb("write");
    int cb; uint32_t off; ReadLoc(t.get("desc"), &cb, &off); x.descOff = off;
    if (x.nvnIndex < 0 && off >= 0x110) x.nvnIndex = (int)((off - 0x110) / 16);     // desc.offset is bytes into c[0]
    m.ssbos.push_back(x);
  }
  if (auto* a = j.get("inputs")) for (auto& t : a->a) {
    uint32_t loc = (uint32_t)t.geti("location");
    ManifestInput x{loc, (uint32_t)t.geti("attr", (int)loc)};      // reuse-v1 fragment inputs carry no `attr`: generic attribute i is location i
    x.type = TypeFromStr(t.gets("type")); m.inputs.push_back(x);
  }
  if (auto* a = j.get("outputs")) for (auto& t : a->a) m.outLocs.push_back((uint32_t)t.geti("location"));
  if (auto* f = j.get("frag")) { m.rtMask = (uint32_t)f->geti("rt_mask", 1); m.writesDepth = f->getb("writes_depth"); }
  if (auto* a = j.get("local_size")) for (size_t i = 0; i < 3 && i < a->a.size(); i++) m.localSize[i] = (uint32_t)a->a[i].n;
  if (auto* r = j.get("requires")) m.subgroup32 = r->getb("subgroup_size_32");
  if (auto* sbuf = j.get("support_buffer")) if (sbuf->t == J::Obj) { m.supportBinding = sbuf->geti("binding", 0); m.supportSize = (uint32_t)sbuf->geti("size", 1600); }
  if (auto* a = j.get("capabilities")) for (auto& c : a->a) m.caps.push_back((uint32_t)c.n);
  if (auto* a = j.get("extensions")) for (auto& c : a->a) m.exts.push_back(c.s);
  if (auto* a = j.get("builtins_in")) for (auto& c : a->a) m.builtinsIn.push_back(c.s);
  if (auto* a = j.get("builtins_out")) for (auto& c : a->a) m.builtinsOut.push_back(c.s);
  m.needsLayerOrVp = std::find(m.builtinsOut.begin(), m.builtinsOut.end(), "layer") != m.builtinsOut.end() ||
                     std::find(m.builtinsOut.begin(), m.builtinsOut.end(), "viewport_index") != m.builtinsOut.end();
  return !m.key.empty();
}
}  // namespace

// ---- cache ----------------------------------------------------------------------------------------------------
namespace {
struct IndexEnt { int stage; bool ok; std::string reason, spv, meta; };
std::string g_root;
std::unordered_map<std::string, IndexEnt> g_index;
std::vector<std::string> g_order;               // index.jsonl order
std::unordered_map<std::string, std::unique_ptr<StageProgram>> g_loaded;

// Program constants (c[1]): one 64 KiB zero padded slot per stage program that is drawn, carved out of 16 MiB host visible chunks.
struct ConstChunk { Buf buf; VkDeviceSize used = 0; };
std::vector<ConstChunk> g_constChunks;
constexpr VkDeviceSize kConstSlot = 64 * 1024, kConstChunkSize = 16u << 20;
}  // namespace

const std::string& ShaderCacheRoot() { return g_root; }
size_t ShaderCacheIndexSize() { return g_order.size(); }
bool ShaderCacheIndexAt(size_t i, std::string* key, int* stage, bool* ok) {
  if (i >= g_order.size()) return false;
  const IndexEnt& e = g_index[g_order[i]];
  *key = g_order[i]; *stage = e.stage; *ok = e.ok; return true;
}

bool ShaderCacheLoad(const std::string& root) {
  g_root = root; g_index.clear(); g_order.clear();
  std::string txt = ReadFile(root + "/index.jsonl");
  if (txt.empty()) { Err("shader cache: %s/index.jsonl not found", root.c_str()); return false; }
  std::istringstream is(txt); std::string line; int ok = 0, bad = 0;
  while (std::getline(is, line)) {
    if (line.empty()) continue;
    J j; if (!ParseJson(line, j) || j.t != J::Obj) continue;
    IndexEnt e; e.stage = StageFromStr(j.gets("stage")); e.ok = j.gets("status") == "ok"; e.reason = j.gets("reason");
    e.spv = j.gets("spv"); e.meta = j.gets("meta");
    std::string key = j.gets("key");
    if (!g_index.count(key)) g_order.push_back(key);
    g_index[key] = e; e.ok ? ok++ : bad++;
  }
  Log("shader cache: %d ok, %d failed (%s)", ok, bad, root.c_str());
  return true;
}

void ShaderCacheAdd(const std::string& key, int stage, const std::vector<uint32_t>& spv, const Manifest& m) {
  auto sp = std::make_unique<StageProgram>();
  sp->key = key; sp->man = m; sp->man.key = key; sp->man.stage = stage; sp->spv = spv; sp->ok = true;
  g_loaded[key] = std::move(sp);
}

// Tests: a program is keyed by the SHA-256 of its stage bytes in guest memory, which a test cannot choose. An alias makes the key of
// made up stage bytes resolve to a real cache entry; the constants hook gives an added stage its c[1] bytes.
static std::unordered_map<std::string, std::string> g_alias;
void ShaderCacheAlias(const std::string& alias, const std::string& key) { g_alias[alias] = key; }
void ShaderCacheSetConstants(const std::string& key, const std::vector<uint8_t>& bytes) {
  auto it = g_loaded.find(key);
  if (it != g_loaded.end()) it->second->consts = bytes;
}

// Destroy the Vulkan objects of every loaded stage (device teardown). The SPIR-V stays, so a new device rebuilds them on demand.
void ShaderCacheReleaseVk() {
  for (auto& kv : g_loaded) {
    StageProgram* sp = kv.second.get();
    if (sp->module) g_vk.f.vkDestroyShaderModule(g_vk.dev, sp->module, nullptr);
    if (sp->setLayout) g_vk.f.vkDestroyDescriptorSetLayout(g_vk.dev, sp->setLayout, nullptr);
    sp->module = VK_NULL_HANDLE; sp->setLayout = VK_NULL_HANDLE; sp->constBuf = VK_NULL_HANDLE; sp->constOff = 0;
  }
  for (auto& c : g_constChunks) DestroyBuf(&c.buf);
  g_constChunks.clear();
}

void ShaderCacheDrop(const std::string& key) {
  auto it = g_loaded.find(key);
  if (it == g_loaded.end()) return;
  StageProgram* sp = it->second.get();
  if (sp->module) g_vk.f.vkDestroyShaderModule(g_vk.dev, sp->module, nullptr);
  if (sp->setLayout) g_vk.f.vkDestroyDescriptorSetLayout(g_vk.dev, sp->setLayout, nullptr);
  g_loaded.erase(it);
}

bool ShaderCacheConstants(StageProgram* sp, VkBuffer* buf, VkDeviceSize* off) {
  std::lock_guard<std::recursive_mutex> lk(g_vk.lock);
  if (!sp->constBuf) {
    if (g_constChunks.empty() || g_constChunks.back().used + kConstSlot > kConstChunkSize) {
      ConstChunk c;
      if (!CreateBuf(&c.buf, kConstChunkSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemKind::HostVisible)) { Err("shader cache: cannot allocate the program constants buffer"); return false; }
      g_constChunks.push_back(c);
    }
    ConstChunk& c = g_constChunks.back();
    uint8_t* dst = (uint8_t*)c.buf.map + c.used;
    memset(dst, 0, kConstSlot);
    memcpy(dst, sp->consts.data(), std::min<size_t>(sp->consts.size(), kConstSlot));
    sp->constBuf = c.buf.buf; sp->constOff = c.used; c.used += kConstSlot;
  }
  *buf = sp->constBuf; *off = sp->constOff; return true;
}

// ---- device check -------------------------------------------------------------------------------------------------
// SPIR-V capability numbers the cache emits, and what the device must offer for each (Vulkan 1.3 core feature names).
static bool CapSupported(uint32_t cap, int stage, std::string* why) {
  const auto& f = g_vk.feat.features;
  auto need = [&](bool ok, const char* what) { if (!ok) *why = what; return ok; };
  static const VkShaderStageFlags stageBits[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
  switch (cap) {
    case 0: case 1: return true;                                    // Matrix, Shader
    case 2: return need(f.geometryShader, "geometryShader");
    case 3: return need(f.tessellationShader, "tessellationShader");
    case 9: return need(g_vk.f12.shaderFloat16, "shaderFloat16");
    case 10: return need(f.shaderFloat64, "shaderFloat64");
    case 11: return need(f.shaderInt64, "shaderInt64");
    case 22: return need(f.shaderInt16, "shaderInt16");
    case 23: case 24: return need(f.shaderTessellationAndGeometryPointSize, "shaderTessellationAndGeometryPointSize");
    case 25: return need(f.shaderImageGatherExtended, "shaderImageGatherExtended");
    case 27: case 48: return need(f.shaderStorageImageMultisample, "shaderStorageImageMultisample");
    case 28: return need(f.shaderUniformBufferArrayDynamicIndexing, "shaderUniformBufferArrayDynamicIndexing");
    case 29: return need(f.shaderSampledImageArrayDynamicIndexing, "shaderSampledImageArrayDynamicIndexing");
    case 30: return need(f.shaderStorageBufferArrayDynamicIndexing, "shaderStorageBufferArrayDynamicIndexing");
    case 31: return need(f.shaderStorageImageArrayDynamicIndexing, "shaderStorageImageArrayDynamicIndexing");
    case 32: return need(f.shaderClipDistance, "shaderClipDistance");
    case 33: return need(f.shaderCullDistance, "shaderCullDistance");
    case 34: case 45: return need(f.imageCubeArray, "imageCubeArray");
    case 35: case 52: return need(f.sampleRateShading, "sampleRateShading");
    case 43: case 44: case 46: case 47: case 49: case 50: case 51: return true;   // 1D, buffer images, extended formats, image query, derivative control: core
    case 55: return need(f.shaderStorageImageReadWithoutFormat, "shaderStorageImageReadWithoutFormat");
    case 56: return need(f.shaderStorageImageWriteWithoutFormat, "shaderStorageImageWriteWithoutFormat");
    case 57: return need(f.multiViewport, "multiViewport");
    case 61: case 62: case 63: case 64: case 65: case 66: case 67: case 68: {   // GroupNonUniform*: subgroup operation bits
      uint32_t bit = 1u << (cap - 61);
      if (!need((g_vk.p11.subgroupSupportedOperations & bit) != 0, "subgroup operation")) return false;
      return need(stage >= 0 && stage < 6 && (g_vk.p11.subgroupSupportedStages & stageBits[stage]) != 0, "subgroup operations in this shader stage");
    }
    case 4427: return need(g_vk.f11.shaderDrawParameters, "shaderDrawParameters");
    case 5254: return need(g_vk.f12.shaderOutputViewportIndex && g_vk.f12.shaderOutputLayer, "shaderOutputViewportIndex and shaderOutputLayer");
    default: { char b[48]; snprintf(b, sizeof b, "unknown SPIR-V capability %u", cap); *why = b; return false; }
  }
}

bool ShaderCacheCheckDevice(StageProgram* sp) {
  if (sp->supportChecked || !sp->ok) return sp->ok;
  sp->supportChecked = true;
  std::string why;
  for (uint32_t c : sp->man.caps) {
    if (!CapSupported(c, sp->man.stage, &why)) { sp->ok = false; sp->reason = "device lacks " + why + " (capability " + std::to_string(c) + ")"; break; }
  }
  if (sp->ok) for (auto& e : sp->man.exts) {
    if (e == "SPV_EXT_shader_viewport_index_layer") { if (!(g_vk.f12.shaderOutputViewportIndex && g_vk.f12.shaderOutputLayer)) { sp->ok = false; sp->reason = "device lacks shaderOutputViewportIndex/shaderOutputLayer (" + e + ")"; break; } }
    else if (e.compare(0, 4, "SPV_") != 0) { sp->ok = false; sp->reason = "unknown SPIR-V extension " + e; break; }
    else LogOnce(("spvext" + e).c_str(), "shader cache: SPIR-V extension %s is not checked against the device", e.c_str());
  }
  if (sp->ok) {
    for (auto& t : sp->man.texs) if (t.res == RES_UNIFORM_TEXEL_BUFFER || t.res == RES_STORAGE_TEXEL_BUFFER || t.dim == 6) { sp->ok = false; sp->reason = "uses texel buffers (not supported)"; break; }
    for (auto& i : sp->man.imgs) if (i.res == RES_STORAGE_TEXEL_BUFFER || i.dim == 6) { sp->ok = false; sp->reason = "uses texel buffer images (not supported)"; break; }
  }
  if (sp->ok && sp->man.subgroup32 && g_vk.p11.subgroupSize != 32 && !(g_vk.f13.subgroupSizeControl && g_vk.p13.minSubgroupSize <= 32 && g_vk.p13.maxSubgroupSize >= 32))
    LogOnce("sg32", "shader cache: the device subgroup size is %u and cannot be forced to 32; lane based shader math may differ", g_vk.p11.subgroupSize);
  if (!sp->ok) LogOnce(sp->key.c_str(), "shader %s: %s (draws using it are skipped)", sp->key.c_str(), sp->reason.c_str());
  return sp->ok;
}

double g_loadReadSec = 0, g_loadParseSec = 0;
void ShaderCacheLoadTimes(double* readSec, double* parseSec) { *readSec = g_loadReadSec; *parseSec = g_loadParseSec; }

StageProgram* ShaderCacheGet(const std::string& keyIn) {
  auto al = g_alias.find(keyIn);
  const std::string& key = al != g_alias.end() ? al->second : keyIn;
  auto it = g_loaded.find(key);
  if (it != g_loaded.end()) return it->second.get();
  auto ix = g_index.find(key);
  auto sp = std::make_unique<StageProgram>();
  sp->key = key;
  auto loadConsts = [&]() {
    if (sp->man.constFile.empty()) return;
    std::string c = ReadFile(g_root + "/" + sp->man.constFile);
    if (c.empty() || c.size() > 64 * 1024) { sp->ok = false; sp->reason = "program constants file missing or unreadable: " + sp->man.constFile; return; }
    sp->consts.assign(c.begin(), c.end());
  };
  if (ix == g_index.end()) {
    // hand written replacement
    std::string rp = g_root + "/replace/" + key;
    std::string spvb = ReadFile(rp + ".spv"), mj = ReadFile(rp + ".json");
    if (!g_root.empty() && !spvb.empty() && !mj.empty()) {
      J j; if (ParseJson(mj, j) && ParseManifest(j, sp->man)) {
        sp->spv.resize(spvb.size() / 4); memcpy(sp->spv.data(), spvb.data(), sp->spv.size() * 4); sp->ok = true; loadConsts();
      }
    }
    if (!sp->ok && sp->reason.empty()) sp->reason = "not in shader cache";
  } else if (!ix->second.ok) {
    std::string rp = g_root + "/replace/" + key;
    std::string spvb = ReadFile(rp + ".spv"), mj = ReadFile(rp + ".json");
    J j;
    if (!spvb.empty() && !mj.empty() && ParseJson(mj, j) && ParseManifest(j, sp->man)) {
      sp->spv.resize(spvb.size() / 4); memcpy(sp->spv.data(), spvb.data(), sp->spv.size() * 4); sp->ok = true; loadConsts();
    } else sp->reason = "translation failed: " + ix->second.reason;
  } else {
    LARGE_INTEGER q0, q1, q2, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    std::string spvb = ReadFile(g_root + "/" + ix->second.spv), mj = ReadFile(g_root + "/" + ix->second.meta);
    QueryPerformanceCounter(&q1);
    J j;
    bool parsed = spvb.size() >= 20 && ParseJson(mj, j) && ParseManifest(j, sp->man);
    QueryPerformanceCounter(&q2);
    g_loadReadSec += (double)(q1.QuadPart - q0.QuadPart) / qf.QuadPart; g_loadParseSec += (double)(q2.QuadPart - q1.QuadPart) / qf.QuadPart;
    if (parsed) {
      sp->spv.resize(spvb.size() / 4); memcpy(sp->spv.data(), spvb.data(), sp->spv.size() * 4); sp->ok = true; loadConsts();
    } else sp->reason = "cache files missing or unreadable";
    sp->man.stage = ix->second.stage;
  }
  if (!sp->ok) LogOnce(key.c_str(), "shader %s: %s (draws using it are skipped)", key.c_str(), sp->reason.c_str());
  StageProgram* r = sp.get();
  g_loaded[key] = std::move(sp);
  return r;
}

}  // namespace nvn
