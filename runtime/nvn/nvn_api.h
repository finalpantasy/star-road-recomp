// Star Road Recomp: NVN API layer helpers (task-12). Our own code (D-008).
#pragma once
#include "nvn_core.h"
#include "nvn_proc.h"

namespace nvn {

template <class T> inline T* ImplOf(const void* guest) {
  const ObjHdr* h = (const ObjHdr*)guest;
  return h ? (T*)h->impl : nullptr;
}
inline void SetHdr(void* guest, uint32_t magic, void* impl) {
  ObjHdr* h = (ObjHdr*)guest;
  h->magic = magic; h->pad = 0; h->impl = impl;
}
inline void ClearHdr(void* guest) { ObjHdr* h = (ObjHdr*)guest; h->magic = 0; h->impl = nullptr; }

// NVNbufferRange: { address, size }
struct BufferRange { uint64_t address; uint64_t size; };
// NVNcopyRegion: six ints
struct CopyRegion { int32_t x, y, z, w, h, d; };
// NVNshaderData: { data address, control section pointer }
struct ShaderData { uint64_t data; const void* control; };

}  // namespace nvn
