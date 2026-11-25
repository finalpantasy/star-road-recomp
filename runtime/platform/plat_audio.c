/* Star Road Recomp platform layer (task-22): audio output. Our own code.
 *
 * Muted unless --audio (owner rule 2026-11-24): with audio off nothing here touches SDL's audio subsystem, so no device is ever opened;
 * hle_audio.c keeps its null sink. With --audio the audout PCM goes into an SDL audio stream on the default playback device (SDL converts
 * the Switch's 6 channel layout down to what the device has). The stream's queue depth is what hle_audio.c uses to hand buffers back at
 * the speed the device really plays them. */
#include <SDL3/SDL.h>
#include <stdlib.h>

#include "plat.h"

struct PlatAudioSink {
    SDL_AudioStream *stream;
    uint32_t rate, channels;
    uint64_t written;
};

static int g_enabled;
void plat_audio_set_enabled(int on) { g_enabled = on; }
int plat_audio_enabled(void) { return g_enabled; }

PlatAudioSink *plat_audio_open(uint32_t rate, uint32_t channels) {
    if (!g_enabled) return NULL;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { plat_log("platform: audio subsystem failed: %s", SDL_GetError()); return NULL; }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)channels, (int)rate};
    SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!st) { plat_log("platform: cannot open the audio device: %s", SDL_GetError()); SDL_QuitSubSystem(SDL_INIT_AUDIO); return NULL; }
    PlatAudioSink *s = (PlatAudioSink *)calloc(1, sizeof *s);
    s->stream = st; s->rate = rate; s->channels = channels;
    SDL_ResumeAudioStreamDevice(st);
    plat_log("platform: audio device open (driver %s, %u Hz, %u channels)", SDL_GetCurrentAudioDriver(), rate, channels);
    return s;
}

void plat_audio_write(PlatAudioSink *s, const void *pcm, uint32_t bytes) {
    if (!s || !bytes) return;
    if (SDL_PutAudioStreamData(s->stream, pcm, (int)bytes)) s->written += bytes;
}

uint64_t plat_audio_queued(const PlatAudioSink *s) {
    if (!s) return 0;
    int q = SDL_GetAudioStreamQueued(s->stream);
    return q > 0 ? (uint64_t)q : 0;
}

void plat_audio_pause(PlatAudioSink *s, int paused) {
    if (!s) return;
    if (paused) SDL_PauseAudioStreamDevice(s->stream);
    else SDL_ResumeAudioStreamDevice(s->stream);
}

void plat_audio_close(PlatAudioSink *s) {
    if (!s) return;
    SDL_DestroyAudioStream(s->stream);        /* also closes the device it opened */
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(s);
}
