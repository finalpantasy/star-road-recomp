// Star Road Recomp: NVN shader cache reader (task-12), consumer of docs/SHADER_CACHE.md v1. Our own code (D-008).
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
bool ParseManifest(const J& j, Manifest& m) {
  m.key = j.gets("key"); m.stage = StageFromStr(j.gets("stage"));
  if (auto* s = j.get("sets")) m.set = (uint32_t)s->geti("set", 0);
  if (auto* a = j.get("ubos")) for (auto& u : a->a) m.ubos.push_back({(uint32_t)u.geti("binding"), u.geti("cbuf"), u.geti("nvn_index", -1), (uint32_t)u.geti("size")});
  if (auto* a = j.get("textures")) for (auto& t : a->a) {
    ManifestTex x{}; x.binding = (uint32_t)t.geti("binding"); x.dim = DimFromStr(t.gets("dim")); x.shadow = t.getb("shadow");
    x.arrayLen = (uint32_t)t.geti("array_len", 1); ReadLoc(t.get("handle"), &x.hCbuf, &x.hOff);
    auto* sh = t.get("sampler_handle"); x.hasSampler = sh && sh->t == J::Obj; if (x.hasSampler) ReadLoc(sh, &x.sCbuf, &x.sOff);
    auto* nv = t.get("nvn"); x.kind = 0; x.index = 0; x.samplerIndex = 0;
    if (nv) { std::string k = nv->gets("kind"); x.kind = k == "combined" ? 0 : k == "separate" ? 1 : 2; x.index = nv->geti("index"); x.samplerIndex = nv->geti("sampler_index"); }
    m.texs.push_back(x);
  }
  if (auto* a = j.get("images")) for (auto& t : a->a) {
    ManifestImg x{}; x.binding = (uint32_t)t.geti("binding"); x.dim = DimFromStr(t.gets("dim")); ReadLoc(t.get("handle"), &x.hCbuf, &x.hOff);
    if (auto* nv = t.get("nvn")) x.index = nv->geti("index"); m.imgs.push_back(x);
  }
  if (auto* a = j.get("ssbos")) for (auto& t : a->a) {
    ManifestSsbo x{}; x.binding = (uint32_t)t.geti("binding"); x.nvnIndex = t.geti("nvn_index", -1);
    int cb; uint32_t off; ReadLoc(t.get("desc"), &cb, &off); x.descOff = off; m.ssbos.push_back(x);
  }
  if (auto* a = j.get("inputs")) for (auto& t : a->a) m.inputs.push_back({(uint32_t)t.geti("location"), (uint32_t)t.geti("attr")});
  if (auto* a = j.get("outputs")) for (auto& t : a->a) m.outLocs.push_back((uint32_t)t.geti("location"));
  if (auto* f = j.get("frag")) m.rtMask = (uint32_t)f->geti("rt_mask", 1);
  if (auto* a = j.get("local_size")) for (size_t i = 0; i < 3 && i < a->a.size(); i++) m.localSize[i] = (uint32_t)a->a[i].n;
  if (auto* r = j.get("requires")) m.subgroup32 = r->getb("subgroup_size_32");
  return !m.key.empty();
}
}  // namespace

// ---- cache ----------------------------------------------------------------------------------------------------
namespace {
struct IndexEnt { int stage; bool ok; std::string reason, spv, meta; };
std::string g_root;
std::unordered_map<std::string, IndexEnt> g_index;
std::unordered_map<std::string, std::unique_ptr<StageProgram>> g_loaded;
}  // namespace

bool ShaderCacheLoad(const std::string& root) {
  g_root = root; g_index.clear();
  std::string txt = ReadFile(root + "/index.jsonl");
  if (txt.empty()) { Err("shader cache: %s/index.jsonl not found", root.c_str()); return false; }
  std::istringstream is(txt); std::string line; int ok = 0, bad = 0;
  while (std::getline(is, line)) {
    if (line.empty()) continue;
    J j; if (!ParseJson(line, j) || j.t != J::Obj) continue;
    IndexEnt e; e.stage = StageFromStr(j.gets("stage")); e.ok = j.gets("status") == "ok"; e.reason = j.gets("reason");
    e.spv = j.gets("spv"); e.meta = j.gets("meta");
    g_index[j.gets("key")] = e; e.ok ? ok++ : bad++;
  }
  Log("shader cache: %d ok, %d failed (%s)", ok, bad, root.c_str());
  return true;
}

void ShaderCacheAdd(const std::string& key, int stage, const std::vector<uint32_t>& spv, const Manifest& m) {
  auto sp = std::make_unique<StageProgram>();
  sp->key = key; sp->man = m; sp->man.key = key; sp->man.stage = stage; sp->spv = spv; sp->ok = true;
  g_loaded[key] = std::move(sp);
}

// Destroy the Vulkan objects of every loaded stage (device teardown). The SPIR-V stays, so a new device rebuilds them on demand.
void ShaderCacheReleaseVk() {
  for (auto& kv : g_loaded) {
    StageProgram* sp = kv.second.get();
    if (sp->module) g_vk.f.vkDestroyShaderModule(g_vk.dev, sp->module, nullptr);
    if (sp->setLayout) g_vk.f.vkDestroyDescriptorSetLayout(g_vk.dev, sp->setLayout, nullptr);
    sp->module = VK_NULL_HANDLE; sp->setLayout = VK_NULL_HANDLE;
  }
}

StageProgram* ShaderCacheGet(const std::string& key) {
  auto it = g_loaded.find(key);
  if (it != g_loaded.end()) return it->second.get();
  auto ix = g_index.find(key);
  auto sp = std::make_unique<StageProgram>();
  sp->key = key;
  if (ix == g_index.end()) {
    // hand written replacement
    std::string rp = g_root + "/replace/" + key;
    std::string spvb = ReadFile(rp + ".spv"), mj = ReadFile(rp + ".json");
    if (!g_root.empty() && !spvb.empty() && !mj.empty()) {
      J j; if (ParseJson(mj, j) && ParseManifest(j, sp->man)) {
        sp->spv.resize(spvb.size() / 4); memcpy(sp->spv.data(), spvb.data(), sp->spv.size() * 4); sp->ok = true;
      }
    }
    if (!sp->ok) sp->reason = "not in shader cache";
  } else if (!ix->second.ok) {
    std::string rp = g_root + "/replace/" + key;
    std::string spvb = ReadFile(rp + ".spv"), mj = ReadFile(rp + ".json");
    J j;
    if (!spvb.empty() && !mj.empty() && ParseJson(mj, j) && ParseManifest(j, sp->man)) {
      sp->spv.resize(spvb.size() / 4); memcpy(sp->spv.data(), spvb.data(), sp->spv.size() * 4); sp->ok = true;
    } else sp->reason = "translation failed: " + ix->second.reason;
  } else {
    std::string spvb = ReadFile(g_root + "/" + ix->second.spv), mj = ReadFile(g_root + "/" + ix->second.meta);
    J j;
    if (spvb.size() >= 20 && ParseJson(mj, j) && ParseManifest(j, sp->man)) {
      sp->spv.resize(spvb.size() / 4); memcpy(sp->spv.data(), spvb.data(), sp->spv.size() * 4); sp->ok = true;
    } else sp->reason = "cache files missing or unreadable";
    sp->man.stage = ix->second.stage;
  }
  if (!sp->ok) LogOnce(key.c_str(), "shader %s: %s (draws using it are skipped)", key.c_str(), sp->reason.c_str());
  StageProgram* r = sp.get();
  g_loaded[key] = std::move(sp);
  return r;
}

}  // namespace nvn
