// Star Road Recomp: default overlay hook (task-23). Our own code. Weak: a strong PlatOverlayDraw anywhere else in the executable replaces it.
#include "overlay_hook.h"

extern "C" __attribute__((weak)) void PlatOverlayDraw(const PlatOverlayFrame*) {}
