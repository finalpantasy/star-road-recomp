/* Star Road Recomp texture path: BC1 / BC3 reference decoders, BC7 mode 6 encoder and decoder. Our own code.
 * The BC7 encoder is the ASTC transcode fallback for GPUs without ASTC: it decodes ASTC to RGBA8 first, then
 * encodes every 4x4 block as BC7 mode 6 (7-bit RGBA endpoints plus per-endpoint p-bit, 4-bit indices) using a
 * principal-axis start and least-squares refinement. Mode 6 handles smooth color and alpha well; blocks with two
 * distinct color clusters lose some precision (measured by texture_check, see the report). */
#include "texture.h"
#include <math.h>
#include <string.h>

static const uint8_t W4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

/* ---- BC1 / BC3 ------------------------------------------------------------------------------------------ */

static void bc1_block(const uint8_t* b, uint8_t* px /* 16 rgba */, int force4) {
    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    uint8_t pal[4][4];
    unsigned cs[2] = {c0, c1};
    for (int i = 0; i < 2; i++) {
        unsigned r = cs[i] >> 11, g = (cs[i] >> 5) & 63, bl = cs[i] & 31;
        pal[i][0] = (uint8_t)((r << 3) | (r >> 2)); pal[i][1] = (uint8_t)((g << 2) | (g >> 4)); pal[i][2] = (uint8_t)((bl << 3) | (bl >> 2)); pal[i][3] = 255;
    }
    if (c0 > c1 || force4) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(px + 4 * i, pal[(idx >> (2 * i)) & 3], 4);
}

int sr_tex_decode_bc1(const uint8_t* blocks, uint32_t w, uint32_t h, uint8_t* out) {
    if (!blocks || !out) return SR_TEX_E_ARG;
    uint32_t nbx = (w + 3) / 4, nby = (h + 3) / 4;
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            uint8_t px[64];
            bc1_block(blocks + 8 * (by * nbx + bx), px, 0);
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                    memcpy(out + ((by * 4 + y) * (uint64_t)w + bx * 4 + x) * 4, px + 4 * (y * 4 + x), 4);
        }
    return SR_TEX_OK;
}

int sr_tex_decode_bc3(const uint8_t* blocks, uint32_t w, uint32_t h, uint8_t* out) {
    if (!blocks || !out) return SR_TEX_E_ARG;
    uint32_t nbx = (w + 3) / 4, nby = (h + 3) / 4;
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            const uint8_t* b = blocks + 16 * (by * nbx + bx);
            uint8_t px[64], al[8];
            bc1_block(b + 8, px, 1);
            al[0] = b[0]; al[1] = b[1];
            if (al[0] > al[1]) for (int i = 1; i < 7; i++) al[i + 1] = (uint8_t)(((7 - i) * al[0] + i * al[1]) / 7);
            else { for (int i = 1; i < 5; i++) al[i + 1] = (uint8_t)(((5 - i) * al[0] + i * al[1]) / 5); al[6] = 0; al[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) px[4 * i + 3] = al[(bits >> (3 * i)) & 7];
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                    memcpy(out + ((by * 4 + y) * (uint64_t)w + bx * 4 + x) * 4, px + 4 * (y * 4 + x), 4);
        }
    return SR_TEX_OK;
}

/* ---- BC7 mode 6 ------------------------------------------------------------------------------------------ */

typedef struct { uint64_t lo, hi; int pos; } bitw;
static void put(bitw* w, uint32_t v, int n) {
    for (int i = 0; i < n; i++, w->pos++) {
        uint64_t bit = (v >> i) & 1;
        if (w->pos < 64) w->lo |= bit << w->pos; else w->hi |= bit << (w->pos - 64);
    }
}

static int pal_err(const int ep0[4], const int ep1[4], const uint8_t* px, uint8_t* idx_out, int* total) {
    int pal[16][4];
    for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) pal[i][c] = (ep0[c] * (64 - W4[i]) + ep1[c] * W4[i] + 32) >> 6;
    int tot = 0;
    for (int p = 0; p < 16; p++) {
        int best = 1 << 30, bi = 0;
        for (int i = 0; i < 16; i++) {
            int e = 0;
            for (int c = 0; c < 4; c++) { int d = pal[i][c] - px[4 * p + c]; e += d * d; }
            if (e < best) { best = e; bi = i; }
        }
        idx_out[p] = (uint8_t)bi; tot += best;
    }
    *total = tot;
    return tot;
}

/* quantize a float endpoint to 7 bits + shared p-bit; returns 8-bit effective values and the p-bit */
static int quant_ep(const float e[4], int out8[4], int* pbit) {
    int best = 1 << 30, bp = 0, bq[4] = {0, 0, 0, 0};
    for (int p = 0; p < 2; p++) {
        int q[4], err = 0;
        for (int c = 0; c < 4; c++) {
            int qq = (int)floorf((e[c] - (float)p) / 2.0f + 0.5f);
            if (qq < 0) qq = 0; if (qq > 127) qq = 127;
            q[c] = qq;
            float d = (float)(2 * qq + p) - e[c];
            err += (int)(d * d * 16.0f);
        }
        if (err < best) { best = err; bp = p; for (int c = 0; c < 4; c++) bq[c] = q[c]; }
    }
    for (int c = 0; c < 4; c++) out8[c] = 2 * bq[c] + bp;
    *pbit = bp;
    return best;
}

static void encode_mode6_block(const uint8_t* px, uint8_t* out, int quality) {
    float mean[4] = {0, 0, 0, 0}, mn[4] = {255, 255, 255, 255}, mx[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) { float v = px[4 * i + c]; mean[c] += v; if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v; }
    for (int c = 0; c < 4; c++) mean[c] /= 16.0f;
    float cov[4][4] = {{0}};
    for (int i = 0; i < 16; i++) {
        float d[4]; for (int c = 0; c < 4; c++) d[c] = px[4 * i + c] - mean[c];
        for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) cov[a][b] += d[a] * d[b];
    }
    float ax[4] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], mx[3] - mn[3]};
    float len = sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2] + ax[3]*ax[3]);
    int flat = len < 0.5f;
    if (!flat) {
        for (int c = 0; c < 4; c++) ax[c] /= len;
        for (int it = 0; it < 8; it++) {
            float nx[4];
            for (int a = 0; a < 4; a++) nx[a] = cov[a][0]*ax[0] + cov[a][1]*ax[1] + cov[a][2]*ax[2] + cov[a][3]*ax[3];
            float l2 = sqrtf(nx[0]*nx[0] + nx[1]*nx[1] + nx[2]*nx[2] + nx[3]*nx[3]);
            if (l2 < 1e-6f) break;
            for (int c = 0; c < 4; c++) ax[c] = nx[c] / l2;
        }
    }
    float e0[4], e1[4];
    if (flat) { for (int c = 0; c < 4; c++) e0[c] = e1[c] = mean[c]; }
    else {
        float tmin = 1e9f, tmax = -1e9f;
        for (int i = 0; i < 16; i++) {
            float t = 0; for (int c = 0; c < 4; c++) t += (px[4*i+c] - mean[c]) * ax[c];
            if (t < tmin) tmin = t; if (t > tmax) tmax = t;
        }
        for (int c = 0; c < 4; c++) { e0[c] = mean[c] + ax[c] * tmin; e1[c] = mean[c] + ax[c] * tmax; }
    }
    int best_err = 1 << 30; uint8_t best_idx[16]; int best_q0[4] = {0}, best_q1[4] = {0}, best_p0 = 0, best_p1 = 0;
    int iters = quality > 0 ? 4 : 1;
    for (int it = 0; it < iters; it++) {
        for (int c = 0; c < 4; c++) { if (e0[c] < 0) e0[c] = 0; if (e0[c] > 255) e0[c] = 255; if (e1[c] < 0) e1[c] = 0; if (e1[c] > 255) e1[c] = 255; }
        int q0[4], q1[4], p0, p1;
        quant_ep(e0, q0, &p0); quant_ep(e1, q1, &p1);
        uint8_t idx[16]; int err;
        pal_err(q0, q1, px, idx, &err);
        if (err < best_err) { best_err = err; memcpy(best_idx, idx, 16); memcpy(best_q0, q0, sizeof q0); memcpy(best_q1, q1, sizeof q1); best_p0 = p0; best_p1 = p1; }
        if (err == 0) break;
        /* least squares endpoints for the current indices */
        float a11 = 0, a12 = 0, a22 = 0, b0[4] = {0}, b1[4] = {0};
        for (int i = 0; i < 16; i++) {
            float al = W4[idx[i]] / 64.0f, be = 1.0f - al;
            a11 += be * be; a12 += al * be; a22 += al * al;
            for (int c = 0; c < 4; c++) { b0[c] += be * px[4*i+c]; b1[c] += al * px[4*i+c]; }
        }
        float det = a11 * a22 - a12 * a12;
        if (fabsf(det) < 1e-4f) break;
        for (int c = 0; c < 4; c++) {
            e0[c] = (b0[c] * a22 - b1[c] * a12) / det;
            e1[c] = (b1[c] * a11 - b0[c] * a12) / det;
        }
    }
    /* anchor: index 0 must have its top bit clear */
    if (best_idx[0] >= 8) {
        for (int c = 0; c < 4; c++) { int t = best_q0[c]; best_q0[c] = best_q1[c]; best_q1[c] = t; }
        int t = best_p0; best_p0 = best_p1; best_p1 = t;
        for (int i = 0; i < 16; i++) best_idx[i] = (uint8_t)(15 - best_idx[i]);
    }
    bitw w = {0, 0, 0};
    put(&w, 1u << 6, 7);                                   /* mode 6: six zeros then a one */
    for (int c = 0; c < 4; c++) { put(&w, (uint32_t)best_q0[c] >> 1, 7); put(&w, (uint32_t)best_q1[c] >> 1, 7); }
    put(&w, (uint32_t)best_p0, 1); put(&w, (uint32_t)best_p1, 1);
    for (int i = 0; i < 16; i++) put(&w, best_idx[i], i == 0 ? 3 : 4);
    memcpy(out, &w.lo, 8); memcpy(out + 8, &w.hi, 8);
}

int sr_tex_encode_bc7(const uint8_t* rgba, uint32_t width, uint32_t height, int quality, uint8_t* out) {
    if (!rgba || !out || !width || !height) return SR_TEX_E_ARG;
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            uint8_t px[64];
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t sx = bx * 4 + x < width ? bx * 4 + x : width - 1, sy = by * 4 + y < height ? by * 4 + y : height - 1;
                    memcpy(px + 4 * (y * 4 + x), rgba + ((uint64_t)sy * width + sx) * 4, 4);
                }
            encode_mode6_block(px, out + 16 * (by * nbx + bx), quality);
        }
    return SR_TEX_OK;
}

int sr_tex_decode_bc7(const uint8_t* blocks, uint32_t width, uint32_t height, uint8_t* out) {
    if (!blocks || !out) return SR_TEX_E_ARG;
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;
    int unsupported = 0;
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            const uint8_t* b = blocks + 16 * (by * nbx + bx);
            uint64_t lo, hi; memcpy(&lo, b, 8); memcpy(&hi, b + 8, 8);
            uint8_t px[64];
            if ((lo & 0x7F) == 0x40) {      /* mode 6 */
                int pos = 7;
                #define GETB(n) ({ uint32_t v_; int nn_ = (n); if (pos + nn_ <= 64) v_ = (uint32_t)((lo >> pos) & ((1u << nn_) - 1)); else if (pos >= 64) v_ = (uint32_t)((hi >> (pos - 64)) & ((1u << nn_) - 1)); else v_ = (uint32_t)(((lo >> pos) | (hi << (64 - pos))) & ((1u << nn_) - 1)); pos += nn_; v_; })
                int q0[4], q1[4];
                for (int c = 0; c < 4; c++) { q0[c] = (int)GETB(7); q1[c] = (int)GETB(7); }
                int p0 = (int)GETB(1), p1 = (int)GETB(1);
                for (int c = 0; c < 4; c++) { q0[c] = q0[c] * 2 + p0; q1[c] = q1[c] * 2 + p1; }
                for (int i = 0; i < 16; i++) {
                    int idx = (int)GETB(i == 0 ? 3 : 4);
                    for (int c = 0; c < 4; c++) px[4 * i + c] = (uint8_t)((q0[c] * (64 - W4[idx]) + q1[c] * W4[idx] + 32) >> 6);
                }
                #undef GETB
            } else {
                unsupported++;
                for (int i = 0; i < 16; i++) { px[4*i] = px[4*i+1] = px[4*i+2] = 128; px[4*i+3] = 255; }
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; x++)
                    memcpy(out + ((by * 4 + y) * (uint64_t)width + bx * 4 + x) * 4, px + 4 * (y * 4 + x), 4);
        }
    return unsupported ? SR_TEX_E_FORMAT : SR_TEX_OK;
}
