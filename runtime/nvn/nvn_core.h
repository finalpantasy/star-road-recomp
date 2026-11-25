// Star Road Recomp: NVN on Vulkan, core runtime pieces shared by the API files (task-12). Our own code (D-008).
#pragma once
#include <mutex>

#include "nvn_internal.h"

namespace nvn {

struct CbImpl {
  Device* dev = nullptr;
  std::shared_ptr<CmdList> cur;
  bool recording = false;
  std::vector<uint64_t> produced;          // list ids made by this command buffer (released at the next BeginRecording)
  void* memCb = nullptr; void* memCbData = nullptr;
  uint32_t sepSamplerBase = 0;
};

struct WindowImpl {
  void* native = nullptr;                  // guest value, ignored
  HWND hwnd = nullptr;                     // our own hidden window (never shown)
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swap = VK_NULL_HANDLE;
  std::vector<VkImage> swapImages;
  VkFormat swapFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D swapExtent{};
  std::vector<TextureG*> textures;         // the game's render target textures for this window
  uint32_t numActive = 0, presentInterval = 1, cropX = 0, cropY = 0, cropW = 0, cropH = 0;
  uint32_t nextIndex = 0;
  uint64_t presents = 0;
  VkSemaphore acquireSem = VK_NULL_HANDLE;
  std::vector<VkSemaphore> presentSems;    // one per swapchain image (a present holds its semaphore until the image is acquired again)
  bool useSwapchain = false;
};

struct QueueImpl {
  Device* dev = nullptr;
  VkCommandPool pool = VK_NULL_HANDLE;
  uint64_t lastSignal = 0;
  uint32_t flags = 0;
  struct RS* hw = nullptr;                // the queue's GPU state (bound program, pools, targets, render state): like the hardware it carries over from one
                                          // command buffer to the next. Created at the first submit, freed with the queue.
};

// ---- registries ----------------------------------------------------------------------------------------------
void RegisterPool(Pool* p);
void UnregisterPool(Pool* p);
uint8_t* HostPtr(uint64_t va, uint64_t size);          // CPU pointer for a GPU virtual address inside a pool, or null
bool ResolveBuffer(uint64_t va, uint64_t size, VkBuffer* buf, uint64_t* off);
std::shared_ptr<CmdList> FindList(uint64_t id);
uint64_t RegisterList(std::shared_ptr<CmdList> l);
void ReleaseList(uint64_t id);

// ---- objects ----------------------------------------------------------------------------------------------------
bool PoolCreate(Pool* p);
void PoolDestroy(Pool* p);
bool TextureCreate(TextureG* t, TextureImpl* ti);
void TextureDestroy(TextureImpl* ti);
VkImageView GetView(TextureImpl* t, const TextureG* g, const TextureView* v, bool forAttachment);
bool SamplerCreate(SamplerImpl* s);
void SamplerDestroy(SamplerImpl* s);
bool ProgramSetShadersImpl(ProgramImpl* p, uint32_t n, const uint64_t* dataAddr, const void* const* control);

// ---- thread safety ------------------------------------------------------------------------------------------------
// The game calls NVN from several threads (main, render, loading). Every proc thunk takes this one lock, so the registries, pools and
// pool tables need no locks of their own. A call that blocks on the GPU releases it for the wait (ApiUnlock) so other threads go on.
// Direct calls from the tests do not go through a thunk and are single threaded.
std::mutex& ApiMutex();
bool& ApiHeld();                                       // true on a thread that is inside a thunk
struct ApiGuard {
  bool took = false;
  ApiGuard() { if (!ApiHeld()) { ApiMutex().lock(); ApiHeld() = true; took = true; } }
  ~ApiGuard() { if (took) { ApiHeld() = false; ApiMutex().unlock(); } }
};
struct ApiUnlock {
  bool had = false;
  ApiUnlock() { if (ApiHeld()) { had = true; ApiHeld() = false; ApiMutex().unlock(); } }
  ~ApiUnlock() { if (had) { ApiMutex().lock(); ApiHeld() = true; } }
};

// ---- queue ------------------------------------------------------------------------------------------------------
bool QueueCreate(QueueImpl* q);
void QueueDestroy(QueueImpl* q);
void QueueSubmit(QueueImpl* q, uint32_t n, const uint64_t* handles);
void QueueFinish(QueueImpl* q);
void QueueFence(QueueImpl* q, Sync* s);
void QueuePresent(QueueImpl* q, WindowImpl* w, uint32_t idx);
// Object lifetime: finalizing a pool, texture, sampler or program clears every queue and command list slot that still points at it,
// so a later submit never touches freed memory. A recorded command that names a pool or program checks IsLive when it replays.
void QueueScrub(const void* object);
void LiveAdd(const void* object);
void LiveDel(const void* object);
bool IsLive(const void* object);
bool WindowCreate(WindowImpl* w);
void WindowDestroy(WindowImpl* w);

// ---- configuration and proc resolution ------------------------------------------------------------------------
void Configure(const VkInitOptions& opt);              // before the first DeviceInitialize (tests); env NVN_VALIDATION / NVN_LAYER_PATH otherwise
const VkInitOptions& CurrentOptions();
void SetProcResolver(void* (*fn)(const char*));
void* ResolveProc(const char* name);
// Link anchors: the API files register their procs from static initializers, so a static library must reference each object file.
void LinkAnchorDevice();
void LinkAnchorMem();
void LinkAnchorCmd();

// ---- debug / test hooks ---------------------------------------------------------------------------------------
struct Stats { uint64_t draws = 0, skippedDraws = 0, pipelines = 0, submits = 0, uploads = 0, clears = 0, copies = 0, dispatches = 0, pipelineFails = 0, noProgNull = 0, noProgIncomplete = 0; };
Stats& GetStats();
// Read back a texture subresource into `out` (tightly packed texels of the GPU format). For tests and captures.
bool ReadbackTexture(TextureImpl* t, uint32_t level, uint32_t layer, std::vector<uint8_t>* out, uint32_t* w, uint32_t* h);
void SetWindowHidden(bool hidden);

}  // namespace nvn
