/* Star Road Recomp texture path: ASTC LDR 2D decoder. Our own code (D-008), written from the ASTC format
 * specification (Khronos Data Format Specification, ASTC chapter). Verified bit-exact against ARM astcenc
 * (test oracle only, never linked) by tools/texture_check.
 * Scope: LDR profile, 2D blocks 4x4..12x12, partitions 1..4, dual plane, all LDR color endpoint modes, void extent.
 * HDR endpoint modes and HDR void extents decode to the error color (magenta), as astcenc's LDR profile does. */
#include "texture.h"
#include <stdatomic.h>
#include <string.h>

/* ---- quantization tables ------------------------------------------------------------------------------- */

typedef struct { uint16_t range; uint8_t bits, trits, quints; } quant_t;
static const quant_t QUANT[21] = {
    {2, 1, 0, 0}, {3, 0, 1, 0}, {4, 2, 0, 0}, {5, 0, 0, 1}, {6, 1, 1, 0}, {8, 3, 0, 0}, {10, 1, 0, 1}, {12, 2, 1, 0},
    {16, 4, 0, 0}, {20, 2, 0, 1}, {24, 3, 1, 0}, {32, 5, 0, 0}, {40, 3, 0, 1}, {48, 4, 1, 0}, {64, 6, 0, 0},
    {80, 4, 0, 1}, {96, 5, 1, 0}, {128, 7, 0, 0}, {160, 5, 0, 1}, {192, 6, 1, 0}, {256, 8, 0, 0}
};

static uint8_t g_color_uq[21][256];
static uint8_t g_weight_uq[12][32];
static atomic_int g_init_state;   /* 0 none, 1 running, 2 done */

static uint32_t replicate(uint32_t v, int from_bits, int to_bits) {
    /* replicate the from_bits-wide value across to_bits (MSB first) */
    uint32_t out = 0;
    int shift = to_bits - from_bits;
    while (shift > -from_bits) {
        if (shift >= 0) out |= v << shift; else out |= v >> -shift;
        shift -= from_bits;
    }
    return out & ((1u << to_bits) - 1);
}

static void init_tables(void) {
    for (int q = 0; q < 21; q++) {
        const quant_t* Q = &QUANT[q];
        for (int i = 0; i < Q->range; i++) {
            int n = Q->bits;
            uint32_t m = (uint32_t)i & ((1u << n) - 1), D = (uint32_t)i >> n;
            uint32_t a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1, d = (m >> 3) & 1, e = (m >> 4) & 1, f = (m >> 5) & 1;
            uint32_t v;
            if (!Q->trits && !Q->quints) {
                v = replicate((uint32_t)i, n, 8);
            } else {
                uint32_t C = 0, B = 0, A = a ? 0x1FF : 0;
                if (Q->trits) {
                    static const uint8_t Ct[7] = {0, 204, 93, 44, 22, 11, 5};
                    C = Ct[n];
                    switch (n) {
                    case 1: B = 0; break;
                    case 2: B = b * 278; break;                       /* b000b0bb0 */
                    case 3: B = c * 266 + b * 133; break;             /* cb000cbcb */
                    case 4: B = d * 260 + c * 130 + b * 65; break;    /* dcb000dcb */
                    case 5: B = e * 258 + d * 129 + c * 64 + b * 32; break;   /* edcb000ed */
                    case 6: B = f * 257 + e * 128 + d * 64 + c * 32 + b * 16; break;   /* fedcb000f */
                    }
                } else {
                    static const uint8_t Cq[6] = {0, 113, 54, 26, 13, 6};
                    C = Cq[n];
                    switch (n) {
                    case 1: B = 0; break;
                    case 2: B = b * 268; break;                       /* b0000bb00 */
                    case 3: B = c * 260 + b * 130; break;             /* cb0000cb0 (checked against astcenc) */
                    case 4: B = d * 258 + c * 129 + b * 64; break;    /* dcb0000dc */
                    case 5: B = e * 257 + d * 128 + c * 64 + b * 32; break;   /* edcb0000e */
                    }
                }
                uint32_t T = D * C + B;
                T ^= A;
                v = (A & 0x80) | (T >> 2);
            }
            g_color_uq[q][i] = (uint8_t)v;
        }
    }
    /* weights: only the first 12 quant levels (range <= 32) are legal */
    for (int q = 0; q < 12; q++) {
        const quant_t* Q = &QUANT[q];
        for (int i = 0; i < Q->range; i++) {
            int n = Q->bits;
            uint32_t m = (uint32_t)i & ((1u << n) - 1), D = (uint32_t)i >> n;
            uint32_t a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1;
            uint32_t v;
            if (Q->range == 3) { static const uint8_t t3[3] = {0, 32, 64}; v = t3[i]; }
            else if (Q->range == 5) { static const uint8_t t5[5] = {0, 16, 32, 48, 64}; v = t5[i]; }
            else if (!Q->trits && !Q->quints) {
                v = n == 1 ? (a ? 63 : 0) : replicate((uint32_t)i, n, 6);
                if (v > 32) v++;
            } else {
                uint32_t C, B = 0, A = a ? 0x7F : 0;
                if (Q->trits) {
                    static const uint8_t Ct[4] = {0, 50, 23, 11};
                    C = Ct[n];
                    if (n == 2) B = b * 69;                /* b000b0b */
                    if (n == 3) B = c * 66 + b * 33;       /* cb000cb */
                } else {
                    static const uint8_t Cq[3] = {0, 28, 13};
                    C = Cq[n];
                    if (n == 2) B = b * 66;                /* b0000b0 (checked against astcenc) */
                }
                uint32_t T = D * C + B;
                T ^= A;
                T = (A & 0x20) | (T >> 2);
                v = T;
                if (v > 32) v++;
            }
            g_weight_uq[q][i] = (uint8_t)v;
        }
    }
}

static void ensure_init(void) {
    if (atomic_load_explicit(&g_init_state, memory_order_acquire) == 2) return;
    int expect = 0;
    if (atomic_compare_exchange_strong(&g_init_state, &expect, 1)) {
        init_tables();
        atomic_store_explicit(&g_init_state, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) { }
    }
}

/* ---- bit access ---------------------------------------------------------------------------------------- */

typedef struct { uint64_t lo, hi; } u128;

static inline uint32_t gb(const u128* b, int pos, int n) {
    if (n == 0) return 0;
    uint64_t mask = (n >= 32) ? 0xFFFFFFFFull : ((1ull << n) - 1);
    if (pos >= 128) return 0;
    if (pos >= 64) return (uint32_t)((b->hi >> (pos - 64)) & mask);
    if (pos + n <= 64) return (uint32_t)((b->lo >> pos) & mask);
    return (uint32_t)(((b->lo >> pos) | (b->hi << (64 - pos))) & mask);
}

static uint64_t rev64(uint64_t x) {
    x = ((x >> 1) & 0x5555555555555555ull) | ((x & 0x5555555555555555ull) << 1);
    x = ((x >> 2) & 0x3333333333333333ull) | ((x & 0x3333333333333333ull) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((x & 0x0F0F0F0F0F0F0F0Full) << 4);
    x = ((x >> 8) & 0x00FF00FF00FF00FFull) | ((x & 0x00FF00FF00FF00FFull) << 8);
    x = ((x >> 16) & 0x0000FFFF0000FFFFull) | ((x & 0x0000FFFF0000FFFFull) << 16);
    return (x >> 32) | (x << 32);
}

static int ise_bits(int count, int q) {
    const quant_t* Q = &QUANT[q];
    int bits = count * Q->bits;
    if (Q->trits) bits += (8 * count + 4) / 5;
    else if (Q->quints) bits += (7 * count + 2) / 3;
    return bits;
}

static void decode_trits(uint32_t T, uint8_t t[5]) {
    uint32_t C, t4, t3;
    if (((T >> 2) & 7) == 7) { C = (((T >> 5) & 7) << 2) | (T & 3); t4 = 2; t3 = 2; }
    else {
        C = T & 0x1F;
        if (((T >> 5) & 3) == 3) { t4 = 2; t3 = (T >> 7) & 1; } else { t4 = (T >> 7) & 1; t3 = (T >> 5) & 3; }
    }
    uint32_t t2, t1, t0;
    if ((C & 3) == 3) { t2 = 2; t1 = (C >> 4) & 1; t0 = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1) & 1); }
    else if (((C >> 2) & 3) == 3) { t2 = 2; t1 = 2; t0 = C & 3; }
    else { t2 = (C >> 4) & 1; t1 = (C >> 2) & 3; t0 = C & 3; }
    t[0] = (uint8_t)t0; t[1] = (uint8_t)t1; t[2] = (uint8_t)t2; t[3] = (uint8_t)t3; t[4] = (uint8_t)t4;
}

static void decode_quints(uint32_t Q, uint8_t q[3]) {
    uint32_t q0, q1, q2;
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        q2 = ((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1) & 1) << 1) | (((Q >> 3) & 1) & ~(Q & 1) & 1);
        q1 = 4; q0 = 4;
    } else {
        uint32_t C;
        if (((Q >> 1) & 3) == 3) { q2 = 4; C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1); }
        else { q2 = (Q >> 5) & 3; C = Q & 0x1F; }
        if ((C & 7) == 5) { q1 = 4; q0 = (C >> 3) & 3; }
        else { q1 = (C >> 3) & 3; q0 = C & 7; }
    }
    q[0] = (uint8_t)q0; q[1] = (uint8_t)q1; q[2] = (uint8_t)q2;
}

/* Decode `count` ISE values starting at bit `start` of the 128-bit stream; out[i] = (trit|quint) << bits | m. */
static void ise_decode(const u128* s, int start, int count, int q, uint8_t* out) {
    const quant_t* Q = &QUANT[q];
    const int n = Q->bits;
    int pos = start;
    if (Q->trits) {
        static const uint8_t tw[5] = {2, 2, 1, 2, 1};
        for (int i = 0; i < count; i += 5) {
            int nb = count - i < 5 ? count - i : 5;
            uint32_t m[5], T = 0;
            int tpos = 0;
            for (int j = 0; j < nb; j++) {
                m[j] = gb(s, pos, n); pos += n;
                T |= gb(s, pos, tw[j]) << tpos; pos += tw[j]; tpos += tw[j];
            }
            uint8_t t[5];
            decode_trits(T, t);
            for (int j = 0; j < nb; j++) out[i + j] = (uint8_t)((t[j] << n) | m[j]);
        }
    } else if (Q->quints) {
        static const uint8_t qw[3] = {3, 2, 2};
        for (int i = 0; i < count; i += 3) {
            int nb = count - i < 3 ? count - i : 3;
            uint32_t m[3], Qv = 0;
            int qpos = 0;
            for (int j = 0; j < nb; j++) {
                m[j] = gb(s, pos, n); pos += n;
                Qv |= gb(s, pos, qw[j]) << qpos; pos += qw[j]; qpos += qw[j];
            }
            uint8_t qq[3];
            decode_quints(Qv, qq);
            for (int j = 0; j < nb; j++) out[i + j] = (uint8_t)((qq[j] << n) | m[j]);
        }
    } else {
        for (int i = 0; i < count; i++) { out[i] = (uint8_t)gb(s, pos, n); pos += n; }
    }
}

/* ---- partition function -------------------------------------------------------------------------------- */

static uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p *= 0xEEDE0891u; p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17;
    return p;
}

typedef struct { uint32_t s[12]; uint32_t rnum; } part_ctx;

static void part_init(part_ctx* c, uint32_t seed, int count) {
    seed += (uint32_t)(count - 1) * 1024;
    uint32_t r = hash52(seed);
    c->rnum = r;
    uint32_t s1 = r & 15, s2 = (r >> 4) & 15, s3 = (r >> 8) & 15, s4 = (r >> 12) & 15, s5 = (r >> 16) & 15,
             s6 = (r >> 20) & 15, s7 = (r >> 24) & 15, s8 = (r >> 28) & 15, s9 = (r >> 18) & 15, s10 = (r >> 22) & 15,
             s11 = (r >> 26) & 15, s12 = ((r >> 30) | (r << 2)) & 15;
    s1 *= s1; s2 *= s2; s3 *= s3; s4 *= s4; s5 *= s5; s6 *= s6; s7 *= s7; s8 *= s8; s9 *= s9; s10 *= s10; s11 *= s11; s12 *= s12;
    int sh1, sh2;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    int sh3 = (seed & 0x10) ? sh1 : sh2;
    c->s[0] = s1 >> sh1; c->s[1] = s2 >> sh2; c->s[2] = s3 >> sh1; c->s[3] = s4 >> sh2;
    c->s[4] = s5 >> sh1; c->s[5] = s6 >> sh2; c->s[6] = s7 >> sh1; c->s[7] = s8 >> sh2;
    c->s[8] = s9 >> sh3; c->s[9] = s10 >> sh3; c->s[10] = s11 >> sh3; c->s[11] = s12 >> sh3;
}

static inline int part_pick(const part_ctx* c, int x, int y, int count) {
    const uint32_t* s = c->s; uint32_t r = c->rnum;
    uint32_t a = (s[0] * x + s[1] * y + (r >> 14)) & 0x3F;
    uint32_t b = (s[2] * x + s[3] * y + (r >> 10)) & 0x3F;
    uint32_t cc = (s[4] * x + s[5] * y + (r >> 6)) & 0x3F;
    uint32_t d = (s[6] * x + s[7] * y + (r >> 2)) & 0x3F;
    if (count < 4) d = 0;
    if (count < 3) cc = 0;
    if (a >= b && a >= cc && a >= d) return 0;
    if (b >= cc && b >= d) return 1;
    if (cc >= d) return 2;
    return 3;
}

/* ---- endpoints ----------------------------------------------------------------------------------------- */

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void bit_transfer_signed(int* a, int* b) {   /* a = offset, b = base */
    int bb = *b >> 1, aa = *a;
    bb |= aa & 0x80;
    aa >>= 1; aa &= 0x3F;
    if (aa & 0x20) aa -= 0x40;
    *a = aa; *b = bb;
}

/* Returns 0 on success, 1 for HDR modes (unsupported in LDR). e0/e1 are rgba 0..255. */
static int decode_endpoint(int cem, const int* v, int e0[4], int e1[4]) {
    switch (cem) {
    case 0: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = e1[3] = 255; return 0;
    case 1: {
        int l0 = (v[0] >> 2) | (v[1] & 0xC0);
        int l1 = clamp255(l0 + (v[1] & 0x3F));
        e0[0] = e0[1] = e0[2] = l0; e1[0] = e1[1] = e1[2] = l1; e0[3] = e1[3] = 255; return 0;
    }
    case 4: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = v[2]; e1[3] = v[3]; return 0;
    case 5: {
        int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];
        bit_transfer_signed(&v1, &v0); bit_transfer_signed(&v3, &v2);
        e0[0] = e0[1] = e0[2] = clamp255(v0); e1[0] = e1[1] = e1[2] = clamp255(v0 + v1);
        e0[3] = clamp255(v2); e1[3] = clamp255(v2 + v3); return 0;
    }
    case 6:
        e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e0[3] = 255;
        e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = 255; return 0;
    case 8: {
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        if (s1 >= s0) { e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; }
        else {
            e0[0] = (v[1] + v[5]) >> 1; e0[1] = (v[3] + v[5]) >> 1; e0[2] = v[5];
            e1[0] = (v[0] + v[4]) >> 1; e1[1] = (v[2] + v[4]) >> 1; e1[2] = v[4];
        }
        e0[3] = e1[3] = 255; return 0;
    }
    case 9: {
        int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3], v4 = v[4], v5 = v[5];
        bit_transfer_signed(&v1, &v0); bit_transfer_signed(&v3, &v2); bit_transfer_signed(&v5, &v4);
        if (v1 + v3 + v5 >= 0) {
            e0[0] = v0; e0[1] = v2; e0[2] = v4; e1[0] = v0 + v1; e1[1] = v2 + v3; e1[2] = v4 + v5;
        } else {
            int r1 = v0 + v1, g1 = v2 + v3, b1 = v4 + v5;
            e0[0] = (r1 + b1) >> 1; e0[1] = (g1 + b1) >> 1; e0[2] = b1;
            e1[0] = (v0 + v4) >> 1; e1[1] = (v2 + v4) >> 1; e1[2] = v4;
        }
        for (int i = 0; i < 3; i++) { e0[i] = clamp255(e0[i]); e1[i] = clamp255(e1[i]); }
        e0[3] = e1[3] = 255; return 0;
    }
    case 10:
        e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e0[3] = v[4];
        e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = v[5]; return 0;
    case 12: {
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        if (s1 >= s0) {
            e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = v[6]; e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; e1[3] = v[7];
        } else {
            e0[0] = (v[1] + v[5]) >> 1; e0[1] = (v[3] + v[5]) >> 1; e0[2] = v[5]; e0[3] = v[7];
            e1[0] = (v[0] + v[4]) >> 1; e1[1] = (v[2] + v[4]) >> 1; e1[2] = v[4]; e1[3] = v[6];
        }
        return 0;
    }
    case 13: {
        int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3], v4 = v[4], v5 = v[5], v6 = v[6], v7 = v[7];
        bit_transfer_signed(&v1, &v0); bit_transfer_signed(&v3, &v2); bit_transfer_signed(&v5, &v4); bit_transfer_signed(&v7, &v6);
        if (v1 + v3 + v5 >= 0) {
            e0[0] = v0; e0[1] = v2; e0[2] = v4; e0[3] = v6; e1[0] = v0 + v1; e1[1] = v2 + v3; e1[2] = v4 + v5; e1[3] = v6 + v7;
        } else {
            int r1 = v0 + v1, g1 = v2 + v3, b1 = v4 + v5, a1 = v6 + v7;
            e0[0] = (r1 + b1) >> 1; e0[1] = (g1 + b1) >> 1; e0[2] = b1; e0[3] = a1;
            e1[0] = (v0 + v4) >> 1; e1[1] = (v2 + v4) >> 1; e1[2] = v4; e1[3] = v6;
        }
        for (int i = 0; i < 4; i++) { e0[i] = clamp255(e0[i]); e1[i] = clamp255(e1[i]); }
        return 0;
    }
    default: return 1;   /* HDR (2, 3, 7, 11, 14, 15) */
    }
}

/* ---- block decode -------------------------------------------------------------------------------------- */

static void fill_error(uint16_t* out, int n) {
    for (int i = 0; i < n; i++) { out[4*i] = 0xFFFF; out[4*i+1] = 0; out[4*i+2] = 0xFFFF; out[4*i+3] = 0xFFFF; }
}

int sr_tex_decode_astc_block(const uint8_t blk[16], uint32_t bw, uint32_t bh, int srgb, uint16_t* out) {
    ensure_init();
    const int ntex = (int)(bw * bh);
    u128 b;
    memcpy(&b.lo, blk, 8); memcpy(&b.hi, blk + 8, 8);   /* little-endian host */
    const uint32_t mode = (uint32_t)(b.lo & 0x7FF);

    if ((mode & 0x1FF) == 0x1FC) {                       /* void extent */
        if (mode & 0x200) { fill_error(out, ntex); return 1; }   /* HDR constant color */
        uint32_t s_lo = (uint32_t)((b.lo >> 12) & 0x1FFF), s_hi = (uint32_t)((b.lo >> 25) & 0x1FFF);
        uint32_t t_lo = (uint32_t)((b.lo >> 38) & 0x1FFF), t_hi = (uint32_t)((b.lo >> 51) & 0x1FFF);
        int all1 = s_lo == 0x1FFF && s_hi == 0x1FFF && t_lo == 0x1FFF && t_hi == 0x1FFF;
        if (!all1 && (s_lo >= s_hi || t_lo >= t_hi)) { fill_error(out, ntex); return 1; }
        uint16_t r = (uint16_t)(b.hi & 0xFFFF), g = (uint16_t)((b.hi >> 16) & 0xFFFF), bl = (uint16_t)((b.hi >> 32) & 0xFFFF), a = (uint16_t)((b.hi >> 48) & 0xFFFF);
        for (int i = 0; i < ntex; i++) { out[4*i] = r; out[4*i+1] = g; out[4*i+2] = bl; out[4*i+3] = a; }
        return 0;
    }

    /* block mode */
    uint32_t base_q = (mode >> 4) & 1, H = (mode >> 9) & 1, D = (mode >> 10) & 1, A = (mode >> 5) & 3;
    uint32_t xw, yw;
    if ((mode & 3) != 0) {
        base_q |= (mode & 3) << 1;
        uint32_t B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
        case 0: xw = B + 4; yw = A + 2; break;
        case 1: xw = B + 8; yw = A + 2; break;
        case 2: xw = A + 2; yw = B + 8; break;
        default:
            B &= 1;
            if (mode & 0x100) { xw = B + 2; yw = A + 2; } else { xw = A + 2; yw = B + 6; }
            break;
        }
    } else {
        base_q |= ((mode >> 2) & 3) << 1;
        if (((mode >> 2) & 3) == 0) { fill_error(out, ntex); return 1; }
        uint32_t B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
        case 0: xw = 12; yw = A + 2; break;
        case 1: xw = A + 2; yw = 12; break;
        case 2: xw = A + 6; yw = B + 6; D = 0; H = 0; break;
        default:
            if (((mode >> 5) & 3) == 0) { xw = 6; yw = 10; }
            else if (((mode >> 5) & 3) == 1) { xw = 10; yw = 6; }
            else { fill_error(out, ntex); return 1; }
            D = 0; H = 0;   /* reserved: bits 9/10 are not dual-plane / high-precision in this form */
            break;
        }
    }
    /* the base_q of the special 6x10/10x6/ A+6 forms: spec keeps R bits; keep as decoded */
    int wq = (int)base_q - 2 + 6 * (int)H;
    if (wq < 0 || wq > 11) { fill_error(out, ntex); return 1; }
    const int nplanes = D ? 2 : 1;
    const int nweights = (int)(xw * yw) * nplanes;
    if (xw > bw || yw > bh || nweights > 64) { fill_error(out, ntex); return 1; }
    const int wbits = ise_bits(nweights, wq);
    if (wbits < 24 || wbits > 96) { fill_error(out, ntex); return 1; }

    const int pcount = (int)((b.lo >> 11) & 3) + 1;
    if (pcount == 4 && D) { fill_error(out, ntex); return 1; }

    /* CEM */
    int cems[4];
    int cem_base_bit, extra_bits = 0;
    if (pcount == 1) {
        cems[0] = (int)((b.lo >> 13) & 15);
        cem_base_bit = 17;
    } else {
        extra_bits = 3 * pcount - 4;
        int below = 128 - wbits - extra_bits;
        uint32_t et = gb(&b, 23, 6) | (gb(&b, below, extra_bits) << 6);
        uint32_t cls = et & 3;
        if (cls == 0) { for (int i = 0; i < pcount; i++) cems[i] = (int)((et >> 2) & 15); extra_bits = 0; }
        else {
            int pos = 2; uint32_t bc = cls - 1;
            for (int i = 0; i < pcount; i++) { uint32_t off = (et >> pos) & 1; pos++; cems[i] = (int)((bc + off) << 2); }
            for (int i = 0; i < pcount; i++) { cems[i] |= (int)((et >> pos) & 3); pos += 2; }
        }
        cem_base_bit = 29;
    }
    /* when all partitions share one CEM the extra bits are absent */
    int colors = 0;
    for (int i = 0; i < pcount; i++) colors += ((cems[i] >> 2) + 1) * 2;
    if (colors > 18) { fill_error(out, ntex); return 1; }
    int plane2 = 0;
    int avail = 128 - wbits - extra_bits - cem_base_bit;
    if (D) { avail -= 2; plane2 = (int)gb(&b, 128 - wbits - extra_bits - 2, 2); }
    int cq = -1;
    for (int q = 20; q >= 4; q--) if (ise_bits(colors, q) <= avail) { cq = q; break; }
    if (cq < 0) { fill_error(out, ntex); return 1; }

    uint8_t cvals[32];
    ise_decode(&b, cem_base_bit, colors, cq, cvals);

    int e0[4][4], e1[4][4];
    {
        int pos = 0;
        for (int p = 0; p < pcount; p++) {
            int n = ((cems[p] >> 2) + 1) * 2, v[8];
            for (int i = 0; i < n; i++) v[i] = g_color_uq[cq][cvals[pos + i]];
            pos += n;
            if (decode_endpoint(cems[p], v, e0[p], e1[p])) { fill_error(out, ntex); return 1; }
        }
    }

    /* weights */
    u128 r;
    r.lo = rev64(b.hi); r.hi = rev64(b.lo);
    uint8_t wraw[64];
    ise_decode(&r, 0, nweights, wq, wraw);
    uint8_t w[2][64 + 16];
    memset(w, 0, sizeof w);
    for (int i = 0; i < nweights; i++) w[nplanes == 2 ? (i & 1) : 0][nplanes == 2 ? (i >> 1) : i] = g_weight_uq[wq][wraw[i]];

    const int DS = (int)((1024 + bw / 2) / (bw - 1)), DT = (int)((1024 + bh / 2) / (bh - 1));
    part_ctx pc;
    const uint32_t pseed = (uint32_t)((b.lo >> 13) & 0x3FF);
    if (pcount > 1) part_init(&pc, pseed, pcount);
    const int small = (int)(bw * bh) < 31;

    for (uint32_t t = 0; t < bh; t++) {
        for (uint32_t s = 0; s < bw; s++) {
            int gs = ((DS * (int)s) * ((int)xw - 1) + 32) >> 6;
            int gt = ((DT * (int)t) * ((int)yw - 1) + 32) >> 6;
            int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            int v0 = js + jt * (int)xw;
            int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int wt[2];
            for (int pl = 0; pl < nplanes; pl++) {
                const uint8_t* ww = w[pl];
                wt[pl] = (ww[v0] * w00 + ww[v0 + 1] * w01 + ww[v0 + (int)xw] * w10 + ww[v0 + (int)xw + 1] * w11 + 8) >> 4;
            }
            int part = 0;
            if (pcount > 1) part = part_pick(&pc, small ? (int)s << 1 : (int)s, small ? (int)t << 1 : (int)t, pcount);
            uint16_t* o = out + 4 * (t * bw + s);
            for (int c = 0; c < 4; c++) {
                int wc = (nplanes == 2 && plane2 == c) ? wt[1] : wt[0];
                int c0, c1;
                /* 16-bit endpoint expansion: linear = e * 257, sRGB = (e << 8) | 0x80; the result is truncated to 8 bits. */
                if (srgb) { c0 = (e0[part][c] << 8) | 0x80; c1 = (e1[part][c] << 8) | 0x80; }
                else { c0 = e0[part][c] * 257; c1 = e1[part][c] * 257; }
                o[c] = (uint16_t)((c0 * (64 - wc) + c1 * wc + 32) >> 6);
            }
        }
    }
    return 0;
}

static inline uint8_t to8(uint16_t v, int srgb) {
    (void)srgb;
    return (uint8_t)(v >> 8);
}

int sr_tex_decode_astc_ldr(const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t width, uint32_t height, int srgb,
                           uint8_t* rgba8, uint32_t* error_blocks) {
    if (!blocks || !rgba8 || bw < 4 || bh < 4 || bw > 12 || bh > 12 || !width || !height) return SR_TEX_E_ARG;
    ensure_init();
    uint32_t nbx = (width + bw - 1) / bw, nby = (height + bh - 1) / bh, errs = 0;
    uint16_t tmp[12 * 12 * 4];
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            errs += (uint32_t)sr_tex_decode_astc_block(blocks + 16 * (by * nbx + bx), bw, bh, srgb, tmp);
            uint32_t x0 = bx * bw, y0 = by * bh;
            uint32_t cw = width - x0 < bw ? width - x0 : bw, ch = height - y0 < bh ? height - y0 : bh;
            for (uint32_t y = 0; y < ch; y++) {
                uint8_t* d = rgba8 + ((uint64_t)(y0 + y) * width + x0) * 4;
                const uint16_t* s = tmp + 4 * y * bw;
                for (uint32_t x = 0; x < cw * 4; x++) d[x] = to8(s[x], srgb);
            }
        }
    if (error_blocks) *error_blocks = errs;
    return SR_TEX_OK;
}
