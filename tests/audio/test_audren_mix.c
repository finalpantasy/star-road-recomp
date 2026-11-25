/* Star Road Recomp audren mixer core tests (task-27, AC1). Our own code. Synthetic voices only: nothing comes from the game.
 * Covers gain, channel routing, clipping, buffer completion and chaining, loops, resampling, float and PCM16, and malformed or out-of-range
 * buffers, voices and parameters. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/audio/audren_mix.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; if (_a != _b) { g_fail++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (double)(a), _b = (double)(b); g_checks++; if (fabs(_a - _b) > (tol)) { g_fail++; printf("FAIL %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static AmMixer *new_mixer(uint32_t rate) { AmMixer *m = (AmMixer *)calloc(1, sizeof *m); am_init(m, rate); return m; }
static void fill_const(int16_t *p, uint32_t frames, uint32_t ch, const int16_t *per_ch) {
    for (uint32_t i = 0; i < frames; i++) for (uint32_t c = 0; c < ch; c++) p[i * ch + c] = per_ch[c];
}

static void test_gain_and_routing(void) {
    AmMixer *m = new_mixer(48000);
    int16_t buf[480];
    int16_t out[2 * 480];

    /* mono: equal power to both sides */
    fill_const(buf, 480, 1, (int16_t[]){10000});
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 48000, 1), AM_OK);
    CHECK_EQ(am_voice_queue(m, 0, buf, 480 * 2, 0, 0, 0, 0, 0), AM_OK);
    m->v[0].state = AM_PLAYING;
    am_mix(m, out, 100);
    CHECK_NEAR(out[0], 10000 * 0.70710678, 1); CHECK_NEAR(out[1], 10000 * 0.70710678, 1);
    CHECK_NEAR(out[198], 10000 * 0.70710678, 1);

    /* stereo default routing, then a volume */
    int16_t st[480 * 2];
    fill_const(st, 480, 2, (int16_t[]){8000, -4000});
    CHECK_EQ(am_voice_setup(m, 1, AM_FMT_PCM16, 48000, 2), AM_OK);
    CHECK_EQ(am_voice_queue(m, 1, st, sizeof st, 0, 0, 0, 0, 0), AM_OK);
    m->v[0].state = AM_STOPPED; m->v[1].state = AM_PLAYING;
    am_mix(m, out, 64);
    CHECK_EQ(out[0], 8000); CHECK_EQ(out[1], -4000); CHECK_EQ(out[126], 8000); CHECK_EQ(out[127], -4000);
    m->v[1].volume = 0.5f;
    am_mix(m, out, 16);
    CHECK_EQ(out[0], 4000); CHECK_EQ(out[1], -2000);

    /* swapped and cross-fed channels, muted channel */
    m->v[1].volume = 1.0f;
    memset(m->v[1].gain, 0, sizeof m->v[1].gain);
    m->v[1].gain[0][1] = 1.0f; m->v[1].gain[1][0] = 0.5f;           /* left source to right output, right source (half) to left output */
    am_mix(m, out, 16);
    CHECK_EQ(out[0], -2000); CHECK_EQ(out[1], 8000);
    memset(m->v[1].gain, 0, sizeof m->v[1].gain);                    /* all gains zero: silence, still consuming */
    am_mix(m, out, 4);
    CHECK_EQ(out[0], 0); CHECK_EQ(out[1], 0);

    /* six channel source folded with explicit gains: only channel 4 reaches the left and channel 5 the right */
    int16_t six[100 * 6];
    fill_const(six, 100, 6, (int16_t[]){1000, 2000, 3000, 4000, 5000, 6000});
    CHECK_EQ(am_voice_setup(m, 2, AM_FMT_PCM16, 48000, 6), AM_OK);
    memset(m->v[2].gain, 0, sizeof m->v[2].gain);
    m->v[2].gain[4][0] = 1.0f; m->v[2].gain[5][1] = 1.0f; m->v[2].gain[0][0] = 0.5f;
    CHECK_EQ(am_voice_queue(m, 2, six, sizeof six, 0, 0, 0, 0, 0), AM_OK);
    m->v[1].state = AM_STOPPED; m->v[2].state = AM_PLAYING;
    am_mix(m, out, 10);
    CHECK_EQ(out[0], 5500); CHECK_EQ(out[1], 6000);

    /* paused voices are silent and do not advance */
    m->v[2].state = AM_PAUSED;
    uint64_t before = m->v[2].played_frames;
    am_mix(m, out, 10);
    CHECK_EQ(out[0], 0); CHECK_EQ(out[19], 0); CHECK_EQ(m->v[2].played_frames, before);
    free(m);
}

static void test_clipping(void) {
    AmMixer *m = new_mixer(48000);
    int16_t hi[64], lo[64], out[2 * 64];
    fill_const(hi, 64, 1, (int16_t[]){32000});
    fill_const(lo, 64, 1, (int16_t[]){-32000});
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(am_voice_setup(m, i, AM_FMT_PCM16, 48000, 1), AM_OK);
        m->v[i].gain[0][0] = m->v[i].gain[0][1] = 1.0f;
        CHECK_EQ(am_voice_queue(m, i, i < 2 ? hi : lo, sizeof hi, 0, 0, 0, 0, 0), AM_OK);
        m->v[i].state = AM_PLAYING;
    }
    m->v[2].state = AM_STOPPED;
    am_mix(m, out, 8);                                              /* 32000 + 32000 */
    CHECK_EQ(out[0], 32767); CHECK_EQ(out[15], 32767);
    CHECK_EQ(m->clipped, 16);
    m->v[0].state = AM_STOPPED; m->v[1].state = AM_STOPPED; m->v[2].state = AM_PLAYING; m->v[1].nwave = 0;
    CHECK_EQ(am_voice_queue(m, 1, lo, sizeof lo, 0, 0, 0, 0, 0), AM_OK);
    m->v[1].state = AM_PLAYING;
    am_mix(m, out, 8);                                              /* -32000 - 32000 */
    CHECK_EQ(out[0], -32768);
    /* exactly at full scale is not a clip */
    uint64_t c0 = m->clipped;
    fill_const(hi, 64, 1, (int16_t[]){32767});
    CHECK_EQ(am_voice_setup(m, 5, AM_FMT_PCM16, 48000, 1), AM_OK);
    m->v[5].gain[0][0] = m->v[5].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 5, hi, sizeof hi, 0, 0, 0, 0, 0), AM_OK);
    m->v[5].state = AM_PLAYING; m->v[1].state = m->v[2].state = AM_STOPPED;
    am_mix(m, out, 4);
    CHECK_EQ(out[0], 32767); CHECK_EQ(m->clipped, c0);
    free(m);
}

static void test_completion_and_chaining(void) {
    AmMixer *m = new_mixer(48000);
    int16_t a[100], b[50], out[2 * 400];
    fill_const(a, 100, 1, (int16_t[]){1000});
    fill_const(b, 50, 1, (int16_t[]){2000});
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 48000, 1), AM_OK);
    m->v[0].gain[0][0] = m->v[0].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 0, a, sizeof a, 0, 0, 0, 0, 0), AM_OK);
    CHECK_EQ(am_voice_queue(m, 0, b, sizeof b, 0, 0, 0, 0, 0), AM_OK);
    m->v[0].state = AM_PLAYING;
    am_mix(m, out, 99);
    CHECK_EQ(m->v[0].buffers_done, 0); CHECK(!m->v[0].wave[0].done);
    am_mix(m, out, 1);                                              /* frame 100: the last frame of buffer A; completion is noticed on the next pull */
    am_mix(m, out, 1);
    CHECK_EQ(m->v[0].buffers_done, 1); CHECK(m->v[0].wave[0].done); CHECK(!m->v[0].wave[1].done);
    CHECK_EQ(out[0], 2000);                                         /* continuity: buffer B starts right after A, no gap or repeat */
    am_mix(m, out, 100);
    CHECK_EQ(m->v[0].buffers_done, 2); CHECK(m->v[0].wave[1].done);
    CHECK_EQ(m->v[0].played_frames, 150);
    CHECK_EQ(m->v[0].state, AM_STOPPED);
    CHECK_EQ(out[2 * 48], 2000); CHECK_EQ(out[2 * 49], 0);          /* played 48 more frames of B in the second mix, then silence from frame 49 */
    free(m);

    /* a sub-range of a buffer plays only that range */
    m = new_mixer(48000);
    int16_t ramp[100];
    for (int i = 0; i < 100; i++) ramp[i] = (int16_t)(i * 100);
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 48000, 1), AM_OK);
    m->v[0].gain[0][0] = m->v[0].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 0, ramp, sizeof ramp, 10, 20, 0, 0, 0), AM_OK);
    m->v[0].state = AM_PLAYING;
    am_mix(m, out, 14);
    for (int i = 0; i < 10; i++) CHECK_EQ(out[2 * i], (10 + i) * 100);
    CHECK_EQ(out[2 * 10], 0);
    CHECK_EQ(m->v[0].played_frames, 10);
    free(m);
}

static void test_loops(void) {
    AmMixer *m = new_mixer(48000);
    int16_t ramp[8], out[2 * 64];
    for (int i = 0; i < 8; i++) ramp[i] = (int16_t)((i + 1) * 100);
    /* loop twice over [2,6) then play on to the end at 8 */
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 48000, 1), AM_OK);
    m->v[0].gain[0][0] = m->v[0].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 0, ramp, sizeof ramp, 0, 8, 2, 2, 6), AM_OK);
    m->v[0].state = AM_PLAYING;
    am_mix(m, out, 40);
    int want[] = {100, 200, 300, 400, 500, 600,  300, 400, 500, 600,  300, 400, 500, 600,  700, 800};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) CHECK_EQ(out[2 * i], want[i]);
    CHECK_EQ(out[2 * 16], 0);
    CHECK_EQ(m->v[0].buffers_done, 1);
    /* forever never finishes */
    CHECK_EQ(am_voice_setup(m, 1, AM_FMT_PCM16, 48000, 1), AM_OK);
    m->v[1].gain[0][0] = m->v[1].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 1, ramp, sizeof ramp, 0, 0, -1, 0, 0), AM_OK);
    m->v[1].state = AM_PLAYING; m->v[0].state = AM_STOPPED;
    for (int i = 0; i < 50; i++) am_mix(m, out, 64);
    CHECK_EQ(m->v[1].buffers_done, 0); CHECK_EQ(m->v[1].state, AM_PLAYING); CHECK_EQ(out[2 * 63], ramp[63 % 8]);
    /* invalid loop regions are refused */
    CHECK_EQ(am_voice_setup(m, 2, AM_FMT_PCM16, 48000, 1), AM_OK);
    CHECK_EQ(am_voice_queue(m, 2, ramp, sizeof ramp, 0, 8, 1, 6, 4), AM_ERR_RANGE);     /* start after end */
    CHECK_EQ(am_voice_queue(m, 2, ramp, sizeof ramp, 2, 8, 1, 1, 6), AM_ERR_RANGE);     /* loop starts before the play region */
    CHECK_EQ(am_voice_queue(m, 2, ramp, sizeof ramp, 0, 8, 1, 2, 9), AM_ERR_RANGE);     /* loop ends past the play region */
    free(m);
}

static void test_resample_and_float(void) {
    AmMixer *m = new_mixer(48000);
    int16_t src[100], out[2 * 400];
    for (int i = 0; i < 100; i++) src[i] = (int16_t)(i * 100);
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 24000, 1), AM_OK);
    m->v[0].gain[0][0] = m->v[0].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 0, src, sizeof src, 0, 0, 0, 0, 0), AM_OK);
    m->v[0].state = AM_PLAYING;
    am_mix(m, out, 300);
    CHECK_EQ(out[0], 0); CHECK_EQ(out[2], 50); CHECK_EQ(out[4], 100); CHECK_EQ(out[6], 150);     /* linear interpolation at the half step */
    CHECK_EQ(m->v[0].played_frames, 100);                              /* 100 source frames in 200 output frames */
    CHECK_EQ(out[2 * 199], 9900); CHECK_EQ(out[2 * 200], 0);

    /* pitch 2.0 at the same rate plays twice as fast; bad pitch values mean 1.0 */
    CHECK_EQ(am_voice_setup(m, 1, AM_FMT_PCM16, 48000, 1), AM_OK);
    m->v[1].gain[0][0] = m->v[1].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 1, src, sizeof src, 0, 0, 0, 0, 0), AM_OK);
    m->v[1].pitch = 2.0f; m->v[1].state = AM_PLAYING; m->v[0].state = AM_STOPPED;
    am_mix(m, out, 50);
    CHECK_EQ(out[2], 200); CHECK_EQ(out[2 * 49], 9800);
    CHECK_EQ(m->v[1].played_frames, 98 + 2 > 100 ? 100 : 100);
    CHECK_EQ(am_voice_setup(m, 2, AM_FMT_PCM16, 48000, 1), AM_OK);
    m->v[2].gain[0][0] = m->v[2].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 2, src, sizeof src, 0, 0, 0, 0, 0), AM_OK);
    m->v[2].pitch = NAN; m->v[2].state = AM_PLAYING; m->v[1].state = AM_STOPPED;
    am_mix(m, out, 10);
    CHECK_EQ(out[2], 100); CHECK_EQ(out[2 * 9], 900);
    m->v[2].pitch = -3.0f; m->v[2].state = AM_PLAYING;
    am_mix(m, out, 4);
    CHECK_EQ(out[2], 1100);

    /* float format, with a NaN and an out-of-range sample */
    float fl[8] = {0.5f, -0.5f, NAN, 2.0f, 0.25f, 0.0f, 0.0f, 0.0f};
    CHECK_EQ(am_voice_setup(m, 3, AM_FMT_FLOAT, 48000, 1), AM_OK);
    m->v[3].gain[0][0] = m->v[3].gain[0][1] = 1.0f;
    CHECK_EQ(am_voice_queue(m, 3, fl, sizeof fl, 0, 5, 0, 0, 0), AM_OK);
    m->v[3].state = AM_PLAYING; m->v[2].state = AM_STOPPED;
    uint64_t c0 = m->clipped;
    am_mix(m, out, 6);
    CHECK_EQ(out[0], 16384); CHECK_EQ(out[2], -16384); CHECK_EQ(out[4], 0); CHECK_EQ(out[6], 32767); CHECK_EQ(out[8], 8192); CHECK_EQ(out[10], 0);
    CHECK_EQ(m->clipped, c0 + 2);                                      /* the 2.0 sample clips on both sides */
    free(m);
}

static void test_malformed(void) {
    AmMixer *m = new_mixer(48000);
    int16_t buf[64] = {0}, out[2 * 64];
    /* voice setup */
    CHECK_EQ(am_voice_setup(m, -1, AM_FMT_PCM16, 48000, 1), AM_ERR_ARGS);
    CHECK_EQ(am_voice_setup(m, AM_MAX_VOICES, AM_FMT_PCM16, 48000, 1), AM_ERR_ARGS);
    CHECK_EQ(am_voice_setup(NULL, 0, AM_FMT_PCM16, 48000, 1), AM_ERR_ARGS);
    CHECK_EQ(am_voice_setup(m, 0, 3 /* ADPCM: not supported */, 48000, 1), AM_ERR_FORMAT);
    CHECK_EQ(am_voice_setup(m, 0, 0, 48000, 1), AM_ERR_FORMAT);
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 0, 1), AM_ERR_ARGS);
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 48000, 0), AM_ERR_ARGS);
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 48000, 7), AM_ERR_ARGS);
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 1000000, 1), AM_ERR_ARGS);
    CHECK_EQ(m->refused_voices, 6);
    /* queue */
    CHECK_EQ(am_voice_setup(m, 0, AM_FMT_PCM16, 48000, 2), AM_OK);
    CHECK_EQ(am_voice_queue(m, 0, NULL, 64, 0, 0, 0, 0, 0), AM_ERR_ARGS);
    CHECK_EQ(am_voice_queue(m, 0, buf, 0, 0, 0, 0, 0, 0), AM_ERR_ARGS);
    CHECK_EQ(am_voice_queue(m, 0, buf, 3, 0, 0, 0, 0, 0), AM_ERR_RANGE);              /* less than one stereo frame */
    CHECK_EQ(am_voice_queue(m, 0, buf, 64, 0, 100, 0, 0, 0), AM_ERR_RANGE);           /* end past the data (64 bytes = 16 frames) */
    CHECK_EQ(am_voice_queue(m, 0, buf, 64, 16, 0, 0, 0, 0), AM_ERR_RANGE);            /* start at the end */
    CHECK_EQ(am_voice_queue(m, 0, buf, 64, 8, 4, 0, 0, 0), AM_ERR_RANGE);             /* start after end */
    CHECK_EQ(am_voice_queue(m, 0, buf, 62, 0, 0, 0, 0, 0), AM_OK);                    /* a ragged tail byte count truncates to whole frames: 15 */
    CHECK_EQ(m->v[0].wave[0].frames, 15);
    CHECK_EQ(am_voice_queue(m, 0, buf, 64, 0, 0, 0, 0, 0), AM_OK);
    CHECK_EQ(am_voice_queue(m, 0, buf, 64, 0, 0, 0, 0, 0), AM_OK);
    CHECK_EQ(am_voice_queue(m, 0, buf, 64, 0, 0, 0, 0, 0), AM_OK);
    CHECK_EQ(am_voice_queue(m, 0, buf, 64, 0, 0, 0, 0, 0), AM_ERR_ARGS);              /* the fifth buffer: queue full */
    CHECK_EQ(am_voice_queue(m, AM_MAX_VOICES, buf, 64, 0, 0, 0, 0, 0), AM_ERR_ARGS);      /* index out of range: refused, not a counted buffer */
    CHECK_EQ(am_voice_queue(m, 1, buf, 64, 0, 0, 0, 0, 0), AM_ERR_ARGS);              /* voice 1 was never set up (format 0) */
    CHECK_EQ(m->bad_buffers, 8);     /* NULL, zero size, 3 bytes, end past data, start at end, start after end, queue full, unconfigured voice */
    /* mixing with nothing playing, an unconfigured voice marked playing, zero output rate, zero frames */
    memset(out, 0x55, sizeof out);
    am_mix(m, out, 4);
    CHECK_EQ(out[0], 0); CHECK_EQ(out[7], 0);
    m->v[7].state = AM_PLAYING;                                                         /* no buffers: ignored */
    am_mix(m, out, 4);
    CHECK_EQ(out[0], 0);
    AmMixer *z = new_mixer(0);
    CHECK_EQ(am_voice_setup(z, 0, AM_FMT_PCM16, 48000, 1), AM_OK);
    CHECK_EQ(am_voice_queue(z, 0, buf, 64, 0, 0, 0, 0, 0), AM_OK);
    z->v[0].state = AM_PLAYING;
    am_mix(z, out, 4);                                                                  /* no division by a zero rate */
    CHECK_EQ(out[0], 0);
    free(z);
    am_mix(m, out, 0);
    /* NaN gain and volume cannot poison the output */
    CHECK_EQ(am_voice_setup(m, 2, AM_FMT_PCM16, 48000, 1), AM_OK);
    int16_t loud[16]; for (int i = 0; i < 16; i++) loud[i] = 20000;
    CHECK_EQ(am_voice_queue(m, 2, loud, sizeof loud, 0, 0, 0, 0, 0), AM_OK);
    m->v[2].gain[0][0] = NAN; m->v[2].gain[0][1] = INFINITY; m->v[2].volume = NAN; m->v[2].state = AM_PLAYING;
    m->v[0].state = AM_STOPPED;
    am_mix(m, out, 4);
    CHECK_EQ(out[0], 0); CHECK_EQ(out[1], 0);
    free(m);
}

static void test_chunking(void) {
    /* more frames than the internal block (4096) in one call equals the same frames in small calls */
    enum { N = 10000 };
    static int16_t src[N], o1[2 * N], o2[2 * N];
    for (int i = 0; i < N; i++) src[i] = (int16_t)(sin(i * 0.01) * 12000);
    AmMixer *a = new_mixer(48000), *b = new_mixer(48000);
    AmMixer *both[2] = {a, b};
    for (int k = 0; k < 2; k++) {
        CHECK_EQ(am_voice_setup(both[k], 0, AM_FMT_PCM16, 44100, 1), AM_OK);
        CHECK_EQ(am_voice_queue(both[k], 0, src, sizeof src, 0, 0, 0, 0, 0), AM_OK);
        both[k]->v[0].state = AM_PLAYING;
    }
    am_mix(a, o1, N);
    for (int i = 0; i < N; i += 100) am_mix(b, o2 + 2 * i, 100);
    CHECK(!memcmp(o1, o2, sizeof o1));
    free(a); free(b);
}

int main(void) {
    test_gain_and_routing();
    test_clipping();
    test_completion_and_chaining();
    test_loops();
    test_resample_and_float();
    test_malformed();
    test_chunking();
    printf("audren_mix: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
