// Star Road Recomp platform layer (task-22): hands the SDL window to NVN. Our own code.
// NVN (runtime/nvn/) makes its swapchain on whatever surface this provider gives it; without a provider it uses the hidden Win32 window the
// unit tests rely on. Register before the first nvn device is created (main.c does it right after plat_init).
#include "../nvn/nvn_core.h"
#include "plat.h"

namespace {

bool CreateSurface(void*, VkInstance inst, VkSurfaceKHR* out) {
  uint64_t s = 0;
  if (!plat_create_surface((void*)inst, &s)) return false;
  *out = (VkSurfaceKHR)(uintptr_t)s;
  return true;
}
void DestroySurface(void*, VkInstance inst, VkSurfaceKHR s) { plat_destroy_surface((void*)inst, (uint64_t)(uintptr_t)s); }
bool IsVisible(void*) { return plat_window_visible() != 0; }
void OnPresent(void*) { plat_present_tick(); }

}  // namespace

extern "C" void plat_register_nvn(void) {
  static nvn::WindowProvider p;
  p.user = nullptr;
  p.create_surface = CreateSurface;
  p.destroy_surface = DestroySurface;
  p.is_visible = IsVisible;
  p.on_present = OnPresent;
  nvn::SetWindowProvider(&p);
}
