/* Star Road Recomp audio renderer mixer core (task-27). Our own code. Header only.
 *
 * What it is: a small software mixer for audren voices: PCM16 and float wave buffers (mono to 6 channels) are resampled with linear
 * interpolation, scaled by the voice volume and a per-channel gain matrix, summed in float and clipped to s16 stereo. It knows nothing about
 * the audren update layout: the update parser fills AmVoice and reads back the completion state.
 *
 * What it is not (yet): the guest's own voices. The Star Road guest never opens an audio renderer (it mixes in its own engine, decodes Opus
 * through hwopus and appends PCM to audout, see backlog/tasks/task-27.md), so no voice layout has been traced and nothing here is wired to
 * RequestUpdate. Formats it cannot play (ADPCM and the like) are refused with AM_ERR_FORMAT and counted, never guessed.
 *
 * Output is interleaved s16 stereo at the renderer's rate. Source channels fold into the stereo bus through `gain[src][0..1]`. */
#ifndef SMRPG_AUDREN_MIX_H
#define SMRPG_AUDREN_MIX_H
#include <math.h>
#include <stdint.h>
#include <string.h>

#define AM_MAX_VOICES 256
#define AM_MAX_WAVES 4
#define AM_MAX_SRC_CH 6

enum { AM_FMT_PCM16 = 1, AM_FMT_FLOAT = 2 };
enum { AM_OK = 0, AM_ERR_FORMAT = -1, AM_ERR_ARGS = -2, AM_ERR_RANGE = -3 };
enum { AM_STOPPED = 0, AM_PLAYING = 1, AM_PAUSED = 2 };

typedef struct {
    const void *data;                    /* interleaved samples */
    uint32_t frames;                     /* frames in `data` (size / (channels * sample size)) */
    uint32_t start, end;                 /* play frames [start, end) */
    uint32_t loop_start, loop_end;       /* loop region when `loop` (loop_end == end plays to the end and wraps) */
    int loop;                            /* 0 once, N > 0 plays N extra times, -1 forever */
    uint32_t pos;                        /* internal: frame position inside [start, end) */
    int loops_done;                      /* internal */
    int done;                            /* set by the mixer when the buffer has finished: the guest sees it as completed */
} AmWave;

typedef struct {
    int state;                           /* AM_STOPPED, AM_PLAYING, AM_PAUSED */
    int format;                          /* AM_FMT_* */
    uint32_t src_rate, channels;         /* source rate and channel count (1 to AM_MAX_SRC_CH) */
    float pitch;                         /* 1.0 = source rate */
    float volume;                        /* voice volume */
    float gain[AM_MAX_SRC_CH][2];        /* per source channel gain into left and right */
    AmWave wave[AM_MAX_WAVES];           /* a queue: played in order */
    int nwave;
    uint32_t cur;                        /* queue index being played */
    double frac;                         /* position fraction inside the current frame (source frames) */
    uint64_t played_frames;              /* source frames consumed */
    uint32_t buffers_done;               /* wave buffers completed so far */
} AmVoice;

typedef struct {
    uint32_t out_rate;
    AmVoice v[AM_MAX_VOICES];
    uint64_t clipped;                    /* output samples that hit the s16 limits */
    uint64_t refused_voices, bad_buffers;
} AmMixer;

static void am_init(AmMixer *m, uint32_t out_rate) { memset(m, 0, sizeof *m); m->out_rate = out_rate; }

static int am_fmt_size(int fmt) { return fmt == AM_FMT_PCM16 ? 2 : (fmt == AM_FMT_FLOAT ? 4 : 0); }

/* Configure a voice. Stereo-style defaults: source channel 0 to left, 1 to right, a mono source to both at 0.707 (equal power). */
static int am_voice_setup(AmMixer *m, int idx, int fmt, uint32_t src_rate, uint32_t channels) {
    if (!m || idx < 0 || idx >= AM_MAX_VOICES) return AM_ERR_ARGS;
    AmVoice *v = &m->v[idx];
    memset(v, 0, sizeof *v);
    if (!am_fmt_size(fmt)) { m->refused_voices++; return AM_ERR_FORMAT; }
    if (!src_rate || src_rate > 192000 || channels < 1 || channels > AM_MAX_SRC_CH) { m->refused_voices++; return AM_ERR_ARGS; }
    v->format = fmt; v->src_rate = src_rate; v->channels = channels; v->pitch = 1.0f; v->volume = 1.0f;
    if (channels == 1) { v->gain[0][0] = v->gain[0][1] = 0.70710678f; }
    else { v->gain[0][0] = 1.0f; v->gain[1][1] = 1.0f; }
    return AM_OK;
}

/* Queue a wave buffer: size in bytes, play region in frames (end 0 = whole buffer). Rejects bad pointers, sizes and regions. */
static int am_voice_queue(AmMixer *m, int idx, const void *data, uint64_t size, uint32_t start, uint32_t end, int loop, uint32_t loop_start, uint32_t loop_end) {
    if (!m || idx < 0 || idx >= AM_MAX_VOICES) return AM_ERR_ARGS;
    AmVoice *v = &m->v[idx];
    int ss = am_fmt_size(v->format);
    if (!ss || !data || !size) { m->bad_buffers++; return AM_ERR_ARGS; }
    if (v->nwave >= AM_MAX_WAVES) { m->bad_buffers++; return AM_ERR_ARGS; }
    uint64_t frames = size / ((uint64_t)ss * v->channels);
    if (!frames || frames > 0xFFFFFFFFu) { m->bad_buffers++; return AM_ERR_RANGE; }
    if (!end) end = (uint32_t)frames;
    if (start >= end || end > frames) { m->bad_buffers++; return AM_ERR_RANGE; }
    if (loop) {
        if (!loop_end) loop_end = end;
        if (loop_start >= loop_end || loop_end > end || loop_start < start) { m->bad_buffers++; return AM_ERR_RANGE; }
    }
    AmWave *w = &v->wave[v->nwave++];
    memset(w, 0, sizeof *w);
    w->data = data; w->frames = (uint32_t)frames; w->start = start; w->end = end; w->loop = loop; w->loop_start = loop_start; w->loop_end = loop_end;
    w->pos = start;
    return AM_OK;
}

static float am_sample(const AmVoice *v, const AmWave *w, uint32_t frame, uint32_t ch) {
    size_t i = (size_t)frame * v->channels + ch;
    if (v->format == AM_FMT_PCM16) return (float)((const int16_t *)w->data)[i] * (1.0f / 32768.0f);
    float f; memcpy(&f, (const float *)w->data + i, 4);
    return f == f ? f : 0.0f;                         /* NaN in a float buffer plays as silence */
}

static int am_looping(const AmWave *w) { return w->loop && (w->loop < 0 || w->loops_done < w->loop); }

/* Settle the voice on its next playable frame: handles loop wraps and moves on to the next queued buffer, marking finished ones done.
 * Returns 0 when the queue is exhausted. */
static int am_next_frame(AmVoice *v, uint32_t *wave, uint32_t *frame) {
    for (;;) {
        if (v->cur >= (uint32_t)v->nwave) return 0;
        AmWave *w = &v->wave[v->cur];
        int looping = am_looping(w);
        uint32_t limit = looping ? w->loop_end : w->end;
        if (w->pos < limit) { *wave = v->cur; *frame = w->pos; return 1; }
        if (looping) { w->loops_done++; w->pos = w->loop_start; continue; }      /* loop_start < loop_end, so this always makes progress */
        w->done = 1; v->buffers_done++; v->cur++;
    }
}

static void am_advance(AmVoice *v) {
    v->wave[v->cur].pos++;
    v->played_frames++;
}

/* Mix `frames` output frames of every playing voice into `out` (interleaved s16 stereo, overwritten). */
static void am_mix(AmMixer *m, int16_t *out, uint32_t frames) {
    static float acc[2 * 4096];
    while (frames) {
        uint32_t n = frames > 4096 ? 4096 : frames;
        for (uint32_t i = 0; i < 2 * n; i++) acc[i] = 0.0f;
        for (int vi = 0; vi < AM_MAX_VOICES; vi++) {
            AmVoice *v = &m->v[vi];
            if (v->state != AM_PLAYING || !v->nwave || !m->out_rate) continue;
            double step = (double)v->src_rate / (double)m->out_rate * (double)(v->pitch > 0.0f && v->pitch < 16.0f ? v->pitch : 1.0f);
            float vol = (v->volume == v->volume) ? v->volume : 0.0f;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t wi, f0;
                if (!am_next_frame(v, &wi, &f0)) { v->state = AM_STOPPED; break; }
                const AmWave *w = &v->wave[wi];
                float t = (float)v->frac, l = 0, r = 0;
                int lp = am_looping(w);
                uint32_t f1 = f0 + 1, lim = lp ? w->loop_end : w->end;
                if (f1 >= lim) f1 = lp ? w->loop_start : f0;                     /* no sample past the end: hold the last one */
                for (uint32_t c = 0; c < v->channels; c++) {
                    float s0 = am_sample(v, w, f0, c), s1 = am_sample(v, w, f1, c);
                    float s = s0 + (s1 - s0) * t;
                    l += s * v->gain[c][0]; r += s * v->gain[c][1];
                }
                acc[2 * i] += l * vol; acc[2 * i + 1] += r * vol;
                v->frac += step;
                while (v->frac >= 1.0) {
                    v->frac -= 1.0;
                    uint32_t sw, sf;
                    am_advance(v);
                    if (!am_next_frame(v, &sw, &sf)) break;
                }
            }
        }
        for (uint32_t i = 0; i < 2 * n; i++) {
            float s = acc[i] * 32768.0f;
            if (!(s == s)) s = 0.0f;                         /* a NaN gain or sample must not reach the integer conversion */
            int32_t q = (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f);
            if (s > 32767.0f) { q = 32767; m->clipped++; }
            else if (s < -32768.0f) { q = -32768; m->clipped++; }
            out[i] = (int16_t)q;
        }
        out += 2 * n; frames -= n;
    }
}

#endif
