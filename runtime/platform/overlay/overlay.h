/* Star Road Recomp options overlay (task-16): the C interface the platform layer uses. Our own code.
 *
 * The overlay is a Dear ImGui menu drawn over the presented frame (PlatOverlayDraw, runtime/gfx/upscale/overlay_hook.h) and opened with F1 or
 * L + R + MINUS held together on a controller (scripted input: the token F1). While it is open the game gets no input: overlay_filter_pads
 * zeroes every pad before it reaches the hid shared memory, and what the player presses drives the menu instead. When it is closed nothing
 * is recorded into the frame and the present is the same as in a build without the overlay.
 * Without Dear ImGui at build time (tools/thirdparty/fetch_imgui.py not run) SMRPG_OVERLAY is undefined and every call below is a no-op. */
#pragma once
#include <stdint.h>

#include "../padstate.h"

#ifdef SMRPG_OVERLAY
#ifdef __cplusplus
extern "C" {
#endif
void overlay_init(void);                              /* main thread, first thing in plat_init: reads the options file, applies the startup settings */
void overlay_shutdown(void);
int overlay_is_open(void);
void overlay_toggle(const char *why);                 /* F1, the combo or the scripted token; why goes to the log */
void overlay_open_from_guest_settings(void);          /* guest thread, once when the Settings coroutine starts */
void overlay_filter_pads(PadState pads[PAD_MAX_PLAYERS], uint32_t *connected_mask);   /* main thread, every 5 ms pad publish */
void overlay_event(const void *sdl_event);            /* const SDL_Event *: the mouse reaches the menu while it is open */
void overlay_main_tick(void *sdl_window);             /* main thread: applies requests from the menu that SDL wants on the main thread (window mode, keyboard map) */
const char *overlay_input_map(void);                  /* options input_map ("" = none chosen) */
int overlay_audio_enabled(void);                      /* options audio = on */
int overlay_audio_volume(void);                       /* 0 .. 100 */
int overlay_option(int id);                           /* any option by its OPT_* id (options.h): the current value, read live each call; enum and bool options give the index */
int overlay_unlock_postgame(void);                    /* options gameplay.unlock_postgame = on (task-24): read when a save is loaded or a new game starts */
int overlay_start_fullscreen(void);                   /* options window_mode = fullscreen */
void overlay_note_fullscreen(int on);                 /* F11 and Alt+Enter keep the option in step with the window */
void overlay_pace(void);                              /* once per presented frame, before the frame is recorded: the frame rate cap */
#ifdef __cplusplus
}
#endif
#else
static inline void overlay_init(void) {}
static inline void overlay_shutdown(void) {}
static inline int overlay_is_open(void) { return 0; }
static inline void overlay_toggle(const char *why) { (void)why; }
static inline void overlay_filter_pads(PadState pads[PAD_MAX_PLAYERS], uint32_t *m) { (void)pads; (void)m; }
static inline void overlay_event(const void *e) { (void)e; }
static inline void overlay_main_tick(void *w) { (void)w; }
static inline const char *overlay_input_map(void) { return ""; }
static inline int overlay_audio_enabled(void) { return 0; }
static inline int overlay_audio_volume(void) { return 100; }
static inline int overlay_option(int id) { (void)id; return 0; }
static inline int overlay_unlock_postgame(void) { return 0; }
static inline int overlay_start_fullscreen(void) { return 0; }
static inline void overlay_note_fullscreen(int on) { (void)on; }
static inline void overlay_pace(void) {}
#endif
