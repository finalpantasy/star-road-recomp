// Star Road Recomp: present-time upscaling (task-23). Our own code (the FSR shaders are AMD's, MIT, fetched by tools/thirdparty/fetch_fsr.py).
//
// QueuePresent calls upscale::Record between "acquire the swapchain image" and "overlay + present". With upscale off (the default) Record
// returns false and QueuePresent runs the same blit it always ran, so `off` is byte-identical to a build without this code.
#pragma once
#include "../../nvn/nvn_vk.h"

namespace upscale {

enum class Mode : int { Off = 0, Fsr1 = 1, Fsr2 = 2 };

// What the pass reads and writes. The caller owns the layouts: src is read in GENERAL, dst is in TRANSFER_DST_OPTIMAL on entry and on return.
struct PresentIO {
  VkCommandBuffer cb;
  VkImage src; VkFormat srcFormat; uint32_t srcW, srcH; bool flipY;   // the game's final texture; flipY: NVN lower-left origin
  VkImage dst; VkFormat dstFormat; uint32_t dstW, dstH;               // the swapchain image
  uint64_t frame;                                                     // present number (GPU time samples are logged now and then)
};

Mode CurrentMode();                    // SMRPG_UPSCALE = off | fsr1 | fsr2 (default off; unknown text means off), read once, then SetMode
void SetMode(Mode m);                  // the overlay (task-16) changes it at run time
void SetSharpness(float stops);        // RCAS attenuation in stops: 0 sharpest, 2 mild (default 0.25); SMRPG_UPSCALE_SHARPNESS. < 0 turns RCAS off
void SetTarget(uint32_t w, uint32_t h);// output size of the pass; 0 x 0 (default) = the swapchain extent; SMRPG_UPSCALE_TARGET = WxH (e.g. 3840x2160)
bool Record(const PresentIO& io);      // true: dst holds the final image; false: nothing recorded, the caller does the plain blit
bool WantSwapchainReadback();          // SMRPG_PRESENT_HASH is set: the swapchain needs TRANSFER_SRC usage (checked when it is made)
void DebugAfterOverlay(const PresentIO& io);   // SMRPG_PRESENT_HASH=frames|every:N: logs a hash of the presented image (dst in COLOR_ATTACHMENT_OPTIMAL here)
void Shutdown();                       // frees the pass's Vulkan objects (device idle)
const char* LastError();               // why a requested mode fell back to the blit ("" when none)

}  // namespace upscale
