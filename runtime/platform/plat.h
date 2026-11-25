/* Star Road Recomp platform layer (task-22): the host side of the game on SDL3. Our own code. No SDL types in this header, so the runtime
 * files that use it (main.c, hle_audio.c, plat_nvn.cpp) build without SDL headers.
 *
 *   window   one SDL window, hidden unless --window; the Vulkan surface comes from it (plat_nvn.cpp hands it to NVN)
 *   input    keyboard and gamepads to Switch Pro Controller state, published into the hid shared memory every 5 ms
 *            (hle_hid.c); SMRPG_INPUT / SMRPG_INPUT_FILE script input for tests (docs/PLATFORM.md)
 *   audio    a real device only with --audio: audout PCM goes to an SDL audio stream; otherwise a null sink and no audio subsystem
 * Everything runs on the main thread (SDL needs it); the guest runs on its own threads. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "padstate.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlatOptions {
    int show_window;               /* --window: show the window (default hidden) */
    int audio;                     /* --audio: open a real audio device (default null sink) */
    int fullscreen;                /* --fullscreen with --window */
    const char *input_config;      /* --input-config PATH: keyboard map ini (else SMRPG_INPUT_CONFIG, else input.ini beside the exe, else defaults) */
} PlatOptions;

/* ---- window and loop (main thread) */
int plat_init(const PlatOptions *o);        /* 0 ok; creates the SDL window and subsystems */
void plat_run(void *boot_thread);            /* HANDLE: pumps events and publishes pad state until the handle is signalled */
void plat_shutdown(void);
int plat_window_visible(void);               /* 1 while the window is shown on screen */
void plat_present_tick(void);                /* one presented frame (the scripted input clock) */
uint32_t plat_frame(void);
int plat_create_surface(void *vk_instance, uint64_t *vk_surface);       /* VkInstance, VkSurfaceKHR; 1 ok */
void plat_destroy_surface(void *vk_instance, uint64_t vk_surface);
void plat_register_nvn(void);                /* plat_nvn.cpp: gives the window to NVN; call before the first NVN device is created */

/* ---- pad state sink (hle_hid.c implements it) */
void hle_hid_publish(const PadState pads[PAD_MAX_PLAYERS], uint32_t connected_mask);

/* ---- audio */
typedef struct PlatAudioSink PlatAudioSink;
void plat_audio_set_enabled(int on);
int plat_audio_enabled(void);
PlatAudioSink *plat_audio_open(uint32_t rate, uint32_t channels);        /* int16 interleaved; NULL when audio is off (null sink) or no device */
void plat_audio_write(PlatAudioSink *s, const void *pcm, uint32_t bytes);
uint64_t plat_audio_queued(const PlatAudioSink *s);                       /* bytes written and not yet taken by the device */
void plat_audio_pause(PlatAudioSink *s, int paused);
void plat_audio_close(PlatAudioSink *s);

/* ---- log: the runtime log in the game, stderr in the tests */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
