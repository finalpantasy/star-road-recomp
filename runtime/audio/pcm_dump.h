/* Star Road Recomp offline PCM capture (task-27). Our own code. Header only.
 *
 * SMRPG_AUDIO_DUMP=<file.wav> writes everything the guest hands to audout (before the SDL device, so it works on the dummy driver and with
 * the null sink) as a 16 bit PCM WAV with the channel count and rate of the audout. The RIFF sizes are patched as the file grows, so a run
 * that ends abruptly leaves a readable file. Never committed: captures live on A:. */
#ifndef SMRPG_PCM_DUMP_H
#define SMRPG_PCM_DUMP_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    FILE *f;
    uint32_t rate, channels;
    uint64_t bytes, last_patch;
} PcmDump;

static void pcm_dump_patch(PcmDump *d) {
    if (!d->f) return;
    uint32_t riff = (uint32_t)(36 + d->bytes), data = (uint32_t)d->bytes;
    long pos = ftell(d->f);
    fseek(d->f, 4, SEEK_SET); fwrite(&riff, 4, 1, d->f);
    fseek(d->f, 40, SEEK_SET); fwrite(&data, 4, 1, d->f);
    fseek(d->f, pos, SEEK_SET);
    fflush(d->f);
    d->last_patch = d->bytes;
}

static int pcm_dump_open(PcmDump *d, const char *path, uint32_t rate, uint32_t channels) {
    memset(d, 0, sizeof *d);
    if (!path || !path[0]) return 0;
    d->f = fopen(path, "wb");
    if (!d->f) return -1;
    d->rate = rate; d->channels = channels;
    uint8_t h[44];
    uint32_t fmt = 16, br = rate * channels * 2;
    uint16_t tag = 1, ch = (uint16_t)channels, ba = (uint16_t)(channels * 2), bits = 16;
    memcpy(h, "RIFF", 4); memset(h + 4, 0, 4); memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt, 4); memcpy(h + 20, &tag, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memset(h + 40, 0, 4);
    fwrite(h, 1, 44, d->f);
    pcm_dump_patch(d);
    return 1;
}

static void pcm_dump_write(PcmDump *d, const void *pcm, uint64_t bytes) {
    if (!d->f || !bytes) return;
    fwrite(pcm, 1, (size_t)bytes, d->f);
    d->bytes += bytes;
    if (d->bytes - d->last_patch >= (1u << 20)) pcm_dump_patch(d);
}

static void pcm_dump_close(PcmDump *d) {
    if (!d->f) return;
    pcm_dump_patch(d);
    fclose(d->f);
    d->f = 0;
}

/* Running measurements over the interleaved s16 stream, logged by the HLE once per `window` frames. */
typedef struct {
    uint64_t frames, nonzero_frames, window_frames;
    double sumsq;
    int peak;
} PcmStats;

static void pcm_stats_add(PcmStats *s, const int16_t *pcm, uint64_t frames, uint32_t channels) {
    for (uint64_t i = 0; i < frames; i++) {
        int nz = 0;
        for (uint32_t c = 0; c < channels; c++) {
            int v = pcm[i * channels + c];
            if (v) nz = 1;
            if (v < 0) v = -v;
            if (v > s->peak) s->peak = v;
            s->sumsq += (double)v * v;
        }
        s->nonzero_frames += (uint64_t)nz;
    }
    s->frames += frames; s->window_frames += frames;
}

#endif
