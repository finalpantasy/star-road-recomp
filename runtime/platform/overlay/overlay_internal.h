/* Star Road Recomp options overlay (task-16): what overlay_apply.cpp (options, input, pacing: no ImGui) shares with overlay.cpp (the menu and
 * its drawing). Our own code. */
#pragma once
#include <stdint.h>

#include "options.h"

namespace ovl {

// ---- options: one store, one lock. The menu (the presenting thread) edits, the platform layer (main thread) reads startup values.
void Lock();
void Unlock();
Options& Opts();                       // hold Lock() while using it
void OptionChanged(int id);            // after an edit: applies it (live options) and schedules the save
void ApplyAll();                       // every option, as at startup
void SaveIfDue(bool force);            // writes the options file when something changed (debounced, or now when `force`)
const char* FilePath();

// ---- menu input: what the player pressed, collected on the main thread, consumed by the menu on the presenting thread
struct PadSnapshot {
  uint64_t down;                       // buttons held now (the left stick already folded into the d-pad bits)
  uint64_t pressed;                    // buttons newly pressed since the last Consume, never lost between frames
};
PadSnapshot ConsumePad();
bool IsOpen();
void Close(const char* why);
bool PopEvent(void* sdl_event_out);    // SDL_Event of the mouse, queued while the menu is open

// ---- input maps the menu can choose from (files named input*.ini beside the exe), index 0 is "auto" (empty path)
int ListInputMaps(char out[][OPT_PATH_MAX], int max);

}  // namespace ovl
