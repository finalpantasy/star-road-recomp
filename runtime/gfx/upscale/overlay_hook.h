// Star Road Recomp: the present-time overlay hook (task-23 defines it, task-16 implements it). Our own code.
//
// QueuePresent (runtime/nvn/nvn_replay.cpp) runs, once per presented frame and in this order:
//   1. the game image goes to the swapchain image (faithful blit, or the upscale pass: runtime/gfx/upscale/upscale.h)
//   2. PlatOverlayDraw(&frame): the overlay records its draws into the same command buffer
//   3. the swapchain image goes to PRESENT_SRC and is presented
// The default PlatOverlayDraw (overlay_stub.cpp) is a weak no-op. The overlay supplies a strong definition with the same signature in any
// object linked into the executable; no edit to QueuePresent is needed. The contract is also written in docs/PLATFORM.md and stays stable:
// fields are only ever appended, and `struct_size` tells the overlay how many exist.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlatOverlayFrame {
  uint32_t struct_size;            // sizeof(PlatOverlayFrame) of the caller
  uint32_t frame;                  // 1-based present number of this window (the same count the scripted input clock uses)
  // Vulkan handles, as void* so this header needs no vulkan.h (cast to VkInstance, VkDevice, ...).
  void* instance;                  // VkInstance
  void* physical_device;           // VkPhysicalDevice
  void* device;                    // VkDevice (Vulkan 1.3, dynamic rendering and synchronization2 enabled)
  void* queue;                     // VkQueue the frame is submitted to; do not submit to it from PlatOverlayDraw
  uint32_t queue_family;           // its family index
  uint32_t image_count;            // swapchain image count
  void* get_instance_proc_addr;    // PFN_vkGetInstanceProcAddr (the runtime links no Vulkan import library: load everything through this)
  void* command_buffer;            // VkCommandBuffer, recording, outside any render pass, one-time submit. Record only; never end or submit it.
  void* image;                     // VkImage: the swapchain image this frame presents (usage COLOR_ATTACHMENT | TRANSFER_DST)
  void* image_view;                // VkImageView of that image, format `format`, 2D, one level (valid until the swapchain is rebuilt)
  uint32_t image_index;            // index in the swapchain (0 .. image_count - 1): per-image resources can be keyed by it
  uint32_t format;                 // VkFormat of image and image_view (B8G8R8A8_SRGB when the game draws sRGB, else B8G8R8A8_UNORM)
  uint32_t width, height;          // extent of image in pixels (the window size, or the upscale target)
  uint32_t layout;                 // VkImageLayout the image is in on entry: COLOR_ATTACHMENT_OPTIMAL (it must be there again on return)
  uint32_t game_width, game_height;// size of the game's own final image before any scaling (1280x720 or 1920x1080 on the original)
  uint64_t swapchain_generation;   // changes whenever the swapchain (images, views, extent) was rebuilt: drop caches keyed on the old one
} PlatOverlayFrame;

// Record the overlay for this frame. Called with the NVN lock held, on the thread that presents. Must not block on the GPU. The default does nothing.
void PlatOverlayDraw(const PlatOverlayFrame* frame);

#ifdef __cplusplus
}
#endif
