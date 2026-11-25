/* Star Road Recomp hwopus decoder (task-27). Our own code; the general library libopus (BSD-3) does the Opus decoding.
 *
 * The Switch's hwopus service decodes Opus for the game (the game's audio engine feeds music and voices through it, then mixes the PCM
 * into audout). Before this file the HLE returned silence for every packet, which is why --audio played nothing.
 *
 * Packet format on the wire (nn::codec): u32 big-endian packet size, u32 big-endian final range, then the Opus packet (multistream packets
 * carry self-delimited sub-packets that libopus handles). The service reports how many input bytes it consumed (8 + size) and how many
 * samples per channel it wrote as interleaved s16.
 *
 * libopus-0.dll is loaded with LoadLibrary (like the FFmpeg DLLs of the movie HLE): SMRPG_OPUS_DLL, the exe directory, then the MSYS2
 * ucrt64 bin. A missing DLL keeps the old silent decoder and logs once; nothing here links libopus. Header only so unit tests include it
 * directly. */
#ifndef SMRPG_HWOPUS_DEC_H
#define SMRPG_HWOPUS_DEC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define HWOPUS_MAX_FRAME 5760            /* 120 ms at 48 kHz, libopus' largest frame */
#define HWOPUS_RESET_STATE 4028          /* OPUS_RESET_STATE */

typedef struct {
    HMODULE dll;
    void *(*ms_create)(int32_t fs, int channels, int streams, int coupled, const unsigned char *mapping, int *err);
    void (*ms_destroy)(void *dec);
    int (*ms_decode)(void *dec, const unsigned char *data, int32_t len, int16_t *pcm, int frame_size, int fec);
    int (*ms_ctl)(void *dec, int request, ...);
    int tried;
} HwOpusLib;

static HwOpusLib g_hwopus_lib;

static HMODULE hwopus_try(const char *path) { return path && path[0] ? LoadLibraryExA(path, 0, LOAD_WITH_ALTERED_SEARCH_PATH) : 0; }

/* Returns 1 when libopus is loaded. dir_hint: extra directory to try (the exe directory), may be NULL. */
static int hwopus_lib_load(const char *dir_hint) {
    HwOpusLib *L = &g_hwopus_lib;
    if (L->dll) return 1;
    if (L->tried) return 0;
    L->tried = 1;
    char full[MAX_PATH];
    HMODULE h = hwopus_try(getenv("SMRPG_OPUS_DLL"));
    if (!h && dir_hint && dir_hint[0]) { snprintf(full, sizeof full, "%s\\libopus-0.dll", dir_hint); h = hwopus_try(full); }
    if (!h) {
        const char *sdl = getenv("SMRPG_SDL3_DIR");
        snprintf(full, sizeof full, "%s\\bin\\libopus-0.dll", sdl && sdl[0] ? sdl : "A:\\msys64\\ucrt64");
        h = hwopus_try(full);
    }
    if (!h) return 0;
    *(FARPROC *)&L->ms_create = GetProcAddress(h, "opus_multistream_decoder_create");
    *(FARPROC *)&L->ms_destroy = GetProcAddress(h, "opus_multistream_decoder_destroy");
    *(FARPROC *)&L->ms_decode = GetProcAddress(h, "opus_multistream_decode");
    *(FARPROC *)&L->ms_ctl = GetProcAddress(h, "opus_multistream_decoder_ctl");
    if (!L->ms_create || !L->ms_destroy || !L->ms_decode || !L->ms_ctl) { FreeLibrary(h); return 0; }
    L->dll = h;
    return 1;
}

typedef struct {
    void *dec;                  /* OpusMSDecoder, NULL = silent fallback */
    int rate, channels, streams, coupled;
    int16_t *tmp;               /* one decoded frame */
} HwOpusDec;

/* A single stream decoder is a multistream decoder with one stream: mono (1 stream, 0 coupled) or stereo (1 stream, 1 coupled). */
static int hwopus_dec_open(HwOpusDec *d, int rate, int channels, int streams, int coupled, const uint8_t *mapping) {
    memset(d, 0, sizeof *d);
    d->rate = rate; d->channels = channels; d->streams = streams; d->coupled = coupled;
    if (channels < 1 || channels > 255 || streams < 1 || coupled < 0 || coupled > streams) return -1;
    if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) return -2;
    if (!g_hwopus_lib.dll) return -3;
    unsigned char map[256];
    if (mapping) memcpy(map, mapping, (size_t)channels);
    else for (int i = 0; i < channels; i++) map[i] = (unsigned char)i;
    int err = 0;
    d->dec = g_hwopus_lib.ms_create(rate, channels, streams, coupled, map, &err);
    if (!d->dec || err) { d->dec = 0; return -4; }
    d->tmp = (int16_t *)malloc((size_t)HWOPUS_MAX_FRAME * (size_t)channels * 2);
    if (!d->tmp) { g_hwopus_lib.ms_destroy(d->dec); d->dec = 0; return -5; }
    return 0;
}

static void hwopus_dec_close(HwOpusDec *d) {
    if (d->dec) g_hwopus_lib.ms_destroy(d->dec);
    free(d->tmp);
    memset(d, 0, sizeof *d);
}

/* Decode one framed packet. On success returns the number of samples per channel (>= 0) and sets *consumed to the input bytes used; the
 * interleaved s16 output is in d->tmp (samples * channels * 2 bytes). Errors: -1 short or malformed header, -2 output too small for the
 * decoded frame, -3 no decoder, -4 libopus error. */
static int hwopus_dec_decode(HwOpusDec *d, const uint8_t *in, uint64_t in_size, uint64_t out_cap_bytes, int reset, uint32_t *consumed) {
    if (consumed) *consumed = 0;
    if (!d->dec) return -3;
    if (!in || in_size < 8) return -1;
    uint32_t size = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | in[3];
    if (size > in_size - 8) return -1;
    if (reset) g_hwopus_lib.ms_ctl(d->dec, HWOPUS_RESET_STATE);
    uint64_t cap = out_cap_bytes / ((uint64_t)d->channels * 2);
    if (cap > HWOPUS_MAX_FRAME) cap = HWOPUS_MAX_FRAME;
    /* libopus rejects an output that is smaller than the frame; decode into the full-size scratch and let the caller check the fit */
    int n = g_hwopus_lib.ms_decode(d->dec, in + 8, (int32_t)size, d->tmp, HWOPUS_MAX_FRAME, 0);
    if (n < 0) return -4;
    if ((uint64_t)n > cap) return -2;
    if (consumed) *consumed = 8 + size;
    return n;
}

#endif
