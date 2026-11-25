/* Star Road Recomp hwopus decoder tests (task-27, AC1 for the decode half). Our own code.
 *
 * Encodes known tones with libopus (loaded from the same DLL the runtime uses), wraps each packet in the hwopus framing (u32 BE size, u32 BE
 * final range, data), decodes it through runtime/audio/hwopus_dec.h and measures the result with a Goertzel filter: the expected tone must be
 * there and everything else far below it. Also covers mono, stereo, a three-stream six channel layout, a silent mapped channel, reset,
 * short and malformed packets, a too-small output and the PCM dump and stats helpers. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/audio/hwopus_dec.h"
#include "../../runtime/audio/pcm_dump.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; if (_a != _b) { g_fail++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

/* libopus encoder entry points, only for the tests */
static void *(*ms_enc_create)(int32_t, int, int, int, const unsigned char *, int, int *);
static int (*ms_enc_encode)(void *, const int16_t *, int, unsigned char *, int32_t);
static void (*ms_enc_destroy)(void *);

#define RATE 48000
#define FRAME 960
#define APP_AUDIO 2049

static const double PI = 3.14159265358979323846;

static double goertzel(const int16_t *pcm, int n, int stride, int ch, double freq) {
    double w = 2 * PI * freq / RATE, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { double s = pcm[(size_t)i * stride + ch] + c * s1 - s2; s2 = s1; s1 = s; }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return sqrt(p) / (n / 2.0);        /* amplitude of that sine */
}

static double rms_ch(const int16_t *pcm, int n, int stride, int ch) {
    double a = 0;
    for (int i = 0; i < n; i++) a += (double)pcm[(size_t)i * stride + ch] * pcm[(size_t)i * stride + ch];
    return sqrt(a / n);
}

/* Encode `frames` frames of a per-channel sine and return framed packets (malloc'd), sizes in sz[]. */
typedef struct { uint8_t *p; uint32_t n; } Pkt;
static int make_packets(Pkt *out, int frames, int channels, int streams, int coupled, const unsigned char *map, const double *freqs, double amp) {
    int err = 0;
    void *enc = ms_enc_create(RATE, channels, streams, coupled, map, APP_AUDIO, &err);
    if (!enc || err) return -1;
    int16_t *pcm = (int16_t *)malloc((size_t)FRAME * channels * 2);
    unsigned char buf[4000];
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < FRAME; i++)
            for (int c = 0; c < channels; c++)
                pcm[i * channels + c] = freqs[c] > 0 ? (int16_t)(amp * 32767 * sin(2 * PI * freqs[c] * (double)(f * FRAME + i) / RATE)) : 0;
        int n = ms_enc_encode(enc, pcm, FRAME, buf, sizeof buf);
        if (n < 0) { free(pcm); ms_enc_destroy(enc); return -2; }
        out[f].p = (uint8_t *)malloc((size_t)n + 8);
        out[f].n = (uint32_t)n + 8;
        out[f].p[0] = (uint8_t)(n >> 24); out[f].p[1] = (uint8_t)(n >> 16); out[f].p[2] = (uint8_t)(n >> 8); out[f].p[3] = (uint8_t)n;
        memset(out[f].p + 4, 0, 4);          /* final range: not checked by the decoder */
        memcpy(out[f].p + 8, buf, (size_t)n);
    }
    free(pcm); ms_enc_destroy(enc);
    return 0;
}

static void free_packets(Pkt *p, int n) { for (int i = 0; i < n; i++) free(p[i].p); }

/* Decode all packets and keep the PCM of frames [skip, frames). */
static int16_t *decode_all(HwOpusDec *d, Pkt *p, int frames, int skip, int *out_frames) {
    int16_t *all = (int16_t *)malloc((size_t)frames * FRAME * d->channels * 2);
    int total = 0;
    for (int f = 0; f < frames; f++) {
        uint32_t consumed = 0;
        int n = hwopus_dec_decode(d, p[f].p, p[f].n, (uint64_t)FRAME * d->channels * 2, f == 0, &consumed);
        CHECK_EQ(n, FRAME);
        CHECK_EQ(consumed, p[f].n);
        if (n != FRAME) break;
        if (f >= skip) { memcpy(all + (size_t)total * d->channels, d->tmp, (size_t)n * d->channels * 2); total += n; }
    }
    *out_frames = total;
    return all;
}

static void tone_case(const char *name, int channels, int streams, int coupled, const unsigned char *map, const double *freqs) {
    enum { FR = 30, SKIP = 5 };
    Pkt pk[FR];
    memset(pk, 0, sizeof pk);
    CHECK_EQ(make_packets(pk, FR, channels, streams, coupled, map, freqs, 0.5), 0);
    HwOpusDec d;
    CHECK_EQ(hwopus_dec_open(&d, RATE, channels, streams, coupled, map), 0);
    int n = 0;
    int16_t *pcm = decode_all(&d, pk, FR, SKIP, &n);
    CHECK_EQ(n, (FR - SKIP) * FRAME);
    for (int c = 0; c < channels; c++) {
        double amp = freqs[c] > 0 ? goertzel(pcm, n, channels, c, freqs[c]) : 0, r = rms_ch(pcm, n, channels, c);
        if (freqs[c] > 0) {
            CHECK(amp > 0.40 * 32767 && amp < 0.60 * 32767);           /* 0.5 full scale */
            double expect_rms = amp / sqrt(2.0);
            CHECK(r < expect_rms * 1.05);                             /* the tone is nearly all of the energy */
            CHECK(r > expect_rms * 0.90);
            double other = goertzel(pcm, n, channels, c, freqs[c] * 1.5);
            CHECK(other < amp * 0.02);                                /* 34 dB below on a neighbouring bin */
        } else {
            CHECK(r < 4.0);                                           /* silent channel */
        }
    }
    printf("  %s: %d ch, %d streams, %d samples per channel, tone amplitude ch0 %.0f\n", name, channels, streams, n,
           freqs[0] > 0 ? goertzel(pcm, n, channels, 0, freqs[0]) : 0.0);
    free(pcm); hwopus_dec_close(&d); free_packets(pk, FR);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    CHECK(hwopus_lib_load(0));
    if (!g_hwopus_lib.dll) { printf("FAIL libopus not found (set SMRPG_OPUS_DLL)\n"); return 1; }
    *(FARPROC *)&ms_enc_create = GetProcAddress(g_hwopus_lib.dll, "opus_multistream_encoder_create");
    *(FARPROC *)&ms_enc_encode = GetProcAddress(g_hwopus_lib.dll, "opus_multistream_encode");
    *(FARPROC *)&ms_enc_destroy = GetProcAddress(g_hwopus_lib.dll, "opus_multistream_encoder_destroy");
    CHECK(ms_enc_create && ms_enc_encode && ms_enc_destroy);
    if (!(ms_enc_create && ms_enc_encode && ms_enc_destroy)) return 1;

    { const double f[] = {440}; unsigned char m[] = {0}; tone_case("mono", 1, 1, 0, m, f); }
    { const double f[] = {440, 880}; unsigned char m[] = {0, 1}; tone_case("stereo", 2, 1, 1, m, f); }
    { const double f[] = {300, 500, 700, 900, 1100, 1300}; unsigned char m[] = {0, 1, 2, 3, 4, 5}; tone_case("six channels (3 coupled streams)", 6, 3, 3, m, f); }
    { const double f[] = {600, 0}; unsigned char m[] = {0, 255}; tone_case("mono stream mapped to a silent second channel", 2, 1, 0, m, f); }

    /* malformed input must fail cleanly, and never write or crash */
    {
        const double f[] = {440, 880}; unsigned char m[] = {0, 1};
        Pkt pk[2]; memset(pk, 0, sizeof pk);
        CHECK_EQ(make_packets(pk, 2, 2, 1, 1, m, f, 0.5), 0);
        HwOpusDec d; CHECK_EQ(hwopus_dec_open(&d, RATE, 2, 1, 1, m), 0);
        uint32_t consumed = 99;
        CHECK_EQ(hwopus_dec_decode(&d, pk[0].p, 7, 4000, 0, &consumed), -1);                 /* shorter than the 8 byte header */
        CHECK_EQ(consumed, 0);
        CHECK_EQ(hwopus_dec_decode(&d, pk[0].p, pk[0].n - 1, 4000, 0, &consumed), -1);         /* header size says more than we have */
        CHECK_EQ(hwopus_dec_decode(&d, NULL, 100, 4000, 0, &consumed), -1);
        CHECK_EQ(hwopus_dec_decode(&d, pk[0].p, pk[0].n, 100, 0, &consumed), -2);              /* output buffer too small for a 20 ms frame */
        uint8_t junk[40] = {0, 0, 0, 32, 0, 0, 0, 0};                                         /* size 32, garbage payload */
        for (int i = 8; i < 40; i++) junk[i] = (uint8_t)(i * 37 + 11);
        int r = hwopus_dec_decode(&d, junk, 40, 4000, 0, &consumed);                          /* libopus either errors or decodes noise: no crash */
        CHECK(r == -4 || r >= 0);
        uint8_t zero[8] = {0};                                                                /* size 0: a lost packet, libopus conceals */
        r = hwopus_dec_decode(&d, zero, 8, 4000, 0, &consumed);
        CHECK(r == -4 || r == -2 || r >= 0);
        CHECK_EQ(hwopus_dec_decode(&d, pk[1].p, pk[1].n, 4000, 1, &consumed), FRAME);          /* still healthy after the errors, with a reset */
        CHECK_EQ(consumed, pk[1].n);
        hwopus_dec_close(&d);
        CHECK_EQ(hwopus_dec_decode(&d, pk[1].p, pk[1].n, 4000, 0, &consumed), -3);             /* closed decoder */
        free_packets(pk, 2);
    }
    /* parameter validation */
    {
        HwOpusDec d;
        CHECK(hwopus_dec_open(&d, 44100, 2, 1, 1, NULL) < 0);                                  /* not an Opus rate */
        CHECK(hwopus_dec_open(&d, RATE, 0, 1, 0, NULL) < 0);
        CHECK(hwopus_dec_open(&d, RATE, 2, 1, 2, NULL) < 0);                                   /* more coupled than total streams */
        CHECK(hwopus_dec_open(&d, RATE, 2, 0, 0, NULL) < 0);
    }
    /* PCM dump and stats helpers */
    {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\smrpg_test_dump.wav", getenv("TEMP") ? getenv("TEMP") : ".");
        PcmDump pd; CHECK_EQ(pcm_dump_open(&pd, path, 48000, 2), 1);
        int16_t pcm[8] = {1, -2, 3, -4, 5, -6, 7, -8};
        pcm_dump_write(&pd, pcm, sizeof pcm);
        pcm_dump_patch(&pd);                                          /* readable while still open (an abrupt exit leaves this) */
        FILE *f = fopen(path, "rb"); uint8_t h[44 + 16] = {0};
        CHECK(f != NULL);
        if (f) { size_t got = fread(h, 1, sizeof h, f); CHECK_EQ(got, 44 + 16); fclose(f); }
        CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
        uint32_t riff, data, rate; uint16_t ch;
        memcpy(&riff, h + 4, 4); memcpy(&data, h + 40, 4); memcpy(&rate, h + 24, 4); memcpy(&ch, h + 22, 2);
        CHECK_EQ(data, 16); CHECK_EQ(riff, 36 + 16); CHECK_EQ(rate, 48000); CHECK_EQ(ch, 2);
        CHECK(!memcmp(h + 44, pcm, 16));
        pcm_dump_close(&pd);
        remove(path);
        PcmStats st; memset(&st, 0, sizeof st);
        int16_t s[6] = {0, 0, 100, -300, 0, 0};                       /* three stereo frames: silent, signal, silent */
        pcm_stats_add(&st, s, 3, 2);
        CHECK_EQ(st.frames, 3); CHECK_EQ(st.nonzero_frames, 1); CHECK_EQ(st.peak, 300);
    }
    printf("hwopus: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
