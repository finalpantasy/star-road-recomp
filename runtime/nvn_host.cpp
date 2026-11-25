// Star Road Recomp: host-side glue between the runtime and the NVN library (task-13 step 2). Our own code (D-008).
//
// Two jobs the NVN library leaves to its host:
//   1. load the shader cache (nvn::ShaderCacheLoad takes one root; tools/rtprep/merge_shader_cache.py builds a merged root
//      that holds our own translator's programs and the reused ones for the stages v1 cannot translate yet);
//   2. read a presented frame back and save it as a PNG (captures, not part of the game).
// Nothing from the game is written anywhere but the capture directory the caller names.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "nvn/nvn_api.h"

extern "C" {

int nvn_host_cache_load(const char* root) {
  return nvn::ShaderCacheLoad(root) ? 1 : 0;
}

}  // extern "C"

namespace {

uint32_t g_crc[256];
void CrcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc[n] = c;
  }
}
uint32_t Crc(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = g_crc[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
void Be32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); }
void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  Be32(out, (uint32_t)data.size());
  size_t at = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  Be32(out, Crc(0, out.data() + at, out.size() - at));
}

// 8-bit RGBA to PNG, stored (uncompressed) deflate blocks: big files, but no dependency and exact bytes.
bool WritePng(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  CrcInit();
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(w * 4 + 1) * h);
  for (uint32_t y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  for (size_t pos = 0; pos < raw.size();) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  Be32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, ihdr;
  Be32(ihdr, w); Be32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  return fclose(f) == 0 && ok;
}

}  // namespace

extern "C" {

// Reads back the texture the window shows at `index` and writes it as a PNG. Returns 1 on success; *nonzero gets the number
// of pixels whose RGB is not black (a blank capture is a finding, not a pass).
int nvn_host_save_png(void* window, int index, const char* path, uint32_t* wout, uint32_t* hout, uint64_t* nonzero) {
  nvn::WindowImpl* w = nvn::ImplOf<nvn::WindowImpl>(window);
  if (!w || index < 0 || (size_t)index >= w->textures.size() || !w->textures[index] || !w->textures[index]->impl) return 0;
  nvn::TextureImpl* t = w->textures[index]->impl;
  std::vector<uint8_t> px;
  uint32_t tw = 0, th = 0;
  if (!nvn::ReadbackTexture(t, 0, 0, &px, &tw, &th)) return 0;
  if (px.size() != (size_t)tw * th * 4) return 0;      // only 8888 colour targets are captured
  bool bgra = t->vkFormat == VK_FORMAT_B8G8R8A8_UNORM || t->vkFormat == VK_FORMAT_B8G8R8A8_SRGB;
  uint64_t nz = 0;
  for (size_t i = 0; i < px.size(); i += 4) {
    if (bgra) std::swap(px[i], px[i + 2]);
    if (px[i] | px[i + 1] | px[i + 2]) nz++;
    px[i + 3] = 255;                                    // the swapchain ignores alpha; keep the PNG opaque
  }
  if (wout) *wout = tw;
  if (hout) *hout = th;
  if (nonzero) *nonzero = nz;
  return WritePng(path, px.data(), tw, th) ? 1 : 0;
}

}  // extern "C"
