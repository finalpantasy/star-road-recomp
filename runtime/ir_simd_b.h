/* Advanced SIMD (NEON) vector semantics helpers (task-5, extension simd_b). Our own code.
 * Included at the end of ir_ops.h. Bodies in tools/lifter/ops_simd_b.py call these with the instruction fields as
 * constants (lifted code: clang folds the switches; interpreter: runtime values). Small hot paths are
 * always_inline; bulky or rare ones are noinline and switch at run time.
 * Semantics follow the Arm ARM pseudocode (not the emulator). Saturation sets FPSR.QC (bit 27). */
#pragma once
#include <immintrin.h>

#define VB_INL static inline __attribute__((always_inline))
#define VB_BIG static __attribute__((noinline))
#define VB_QC(c) ((c)->fpsr |= 0x08000000u)

typedef union { v128 v; u8 b[16]; u16 h[8]; u32 s[4]; u64 d[2]; } vb_u;
typedef s8 vb_s8x16 __attribute__((vector_size(16)));
typedef u8 vb_u8x16 __attribute__((vector_size(16)));
typedef s16 vb_s16x8 __attribute__((vector_size(16)));
typedef u16 vb_u16x8 __attribute__((vector_size(16)));
typedef s32 vb_s32x4 __attribute__((vector_size(16)));
typedef u32 vb_u32x4 __attribute__((vector_size(16)));
typedef s64 vb_s64x2 __attribute__((vector_size(16)));
typedef u64 vb_u64x2 __attribute__((vector_size(16)));

VB_INL u64 vb_get(const vb_u *x, int es, int i) {
    switch (es) { case 0: return x->b[i]; case 1: return x->h[i]; case 2: return x->s[i]; default: return x->d[i]; }
}
VB_INL s128 vb_gets(const vb_u *x, int es, int i) {
    switch (es) { case 0: return (s8)x->b[i]; case 1: return (s16)x->h[i]; case 2: return (s32)x->s[i]; default: return (s64)x->d[i]; }
}
VB_INL s128 vb_getx(const vb_u *x, int es, int i, int uns) { return uns ? (s128)vb_get(x, es, i) : vb_gets(x, es, i); }
VB_INL void vb_put(vb_u *x, int es, int i, u64 v) {
    switch (es) { case 0: x->b[i] = (u8)v; break; case 1: x->h[i] = (u16)v; break; case 2: x->s[i] = (u32)v; break; default: x->d[i] = v; break; }
}
VB_INL u64 vb_mask(int ebits) { return ebits >= 64 ? ~0ull : ((1ull << ebits) - 1); }
VB_INL v128 vb_lo(v128 a) { v128 r = {a[0], 0}; return r; }
VB_INL v128 vb_q(int q, v128 a) { return q ? a : vb_lo(a); }
VB_INL v128 vb_zero(void) { v128 r = {0, 0}; return r; }

VB_INL s128 vb_satS(Ctx *c, s128 v, int eb) {
    s128 hi = ((s128)1 << (eb - 1)) - 1, lo = -hi - 1;
    if (v > hi) { VB_QC(c); return hi; }
    if (v < lo) { VB_QC(c); return lo; }
    return v;
}
VB_INL s128 vb_satU(Ctx *c, s128 v, int eb) {
    s128 hi = ((s128)1 << eb) - 1;
    if (v > hi) { VB_QC(c); return hi; }
    if (v < 0) { VB_QC(c); return 0; }
    return v;
}

/* ---------------------------------------------------------------- logic, moves, immediates */
VB_INL v128 vb_logic(int q, int opc, v128 n, v128 m, v128 d) {
    v128 r;
    switch (opc) {
    case 0: r = n & m; break;
    case 1: r = n & ~m; break;
    case 2: r = n | m; break;
    case 3: r = n | ~m; break;
    case 4: r = n ^ m; break;
    case 5: r = (d & n) | (~d & m); break;       /* BSL */
    case 6: r = (d & ~m) | (n & m); break;       /* BIT */
    default: r = (d & m) | (n & ~m); break;      /* BIF */
    }
    return vb_q(q, r);
}
VB_INL v128 vb_movi(int q, u64 imm) { v128 r = {(long long)imm, q ? (long long)imm : 0}; return r; }
VB_INL v128 vb_bici(int q, int bic, u64 imm, v128 d) {
    v128 k = {(long long)imm, (long long)imm};
    return vb_q(q, bic ? (d & ~k) : (d | k));
}

/* ---------------------------------------------------------------- vector-extension fast paths (integer) */
#define VB_DEFBIN(NAME, EXPR)                                                                              \
    VB_INL v128 NAME(int es, int u, v128 a, v128 b) {                                                      \
        if (u) {                                                                                           \
            switch (es) {                                                                                  \
            case 0: { vb_u8x16 x = (vb_u8x16)a, y = (vb_u8x16)b; return (v128)(EXPR); }                    \
            case 1: { vb_u16x8 x = (vb_u16x8)a, y = (vb_u16x8)b; return (v128)(EXPR); }                    \
            case 2: { vb_u32x4 x = (vb_u32x4)a, y = (vb_u32x4)b; return (v128)(EXPR); }                    \
            default: { vb_u64x2 x = (vb_u64x2)a, y = (vb_u64x2)b; return (v128)(EXPR); }                   \
            }                                                                                              \
        }                                                                                                  \
        switch (es) {                                                                                      \
        case 0: { vb_s8x16 x = (vb_s8x16)a, y = (vb_s8x16)b; return (v128)(EXPR); }                        \
        case 1: { vb_s16x8 x = (vb_s16x8)a, y = (vb_s16x8)b; return (v128)(EXPR); }                        \
        case 2: { vb_s32x4 x = (vb_s32x4)a, y = (vb_s32x4)b; return (v128)(EXPR); }                        \
        default: { vb_s64x2 x = (vb_s64x2)a, y = (vb_s64x2)b; return (v128)(EXPR); }                       \
        }                                                                                                  \
    }
VB_DEFBIN(vb_f_add, x + y)
VB_DEFBIN(vb_f_sub, x - y)
VB_DEFBIN(vb_f_gt, x > y)
VB_DEFBIN(vb_f_ge, x >= y)
VB_DEFBIN(vb_f_eq, x == y)
VB_DEFBIN(vb_f_max, __builtin_elementwise_max(x, y))
VB_DEFBIN(vb_f_min, __builtin_elementwise_min(x, y))
VB_DEFBIN(vb_f_mul, x * y)
VB_DEFBIN(vb_f_abd, __builtin_elementwise_max(x, y) - __builtin_elementwise_min(x, y))
VB_DEFBIN(vb_f_tst, ((x & y) != 0))

/* variable shift (SSHL/USHL) on 32-bit lanes via AVX2 */
VB_INL v128 vb_ushl32(int u, v128 n, v128 m) {
    __m128i a = (__m128i)n;
    __m128i sh = _mm_srai_epi32(_mm_slli_epi32((__m128i)m, 24), 24);       /* low byte of each lane, signed */
    __m128i zero = _mm_setzero_si128();
    __m128i left = _mm_sllv_epi32(a, _mm_max_epi32(sh, zero));
    __m128i cnt = _mm_max_epi32(_mm_sub_epi32(zero, sh), zero);
    __m128i right = u ? _mm_srlv_epi32(a, cnt) : _mm_srav_epi32(a, cnt);
    return (v128)_mm_blendv_epi8(right, left, _mm_cmpgt_epi32(sh, _mm_set1_epi32(-1)));
}

/* ---------------------------------------------------------------- three-same, integer */
VB_INL s128 vb_shl_lane(Ctx *c, s128 a, int sh, int eb, int rnd, int sat, int u) {
    if (sh >= 0) {
        if (!sat) return sh >= eb ? 0 : (s128)((u128)a << sh);
        if (a == 0) return 0;
        s128 v;
        if (sh >= eb) v = a < 0 ? -((s128)1 << eb) : ((s128)1 << eb);
        else v = (s128)((u128)a << sh);
        return u ? vb_satU(c, v, eb) : vb_satS(c, v, eb);
    }
    int s = -sh;
    if (s > 120) s = 120;
    return rnd ? ((a + ((s128)1 << (s - 1))) >> s) : (a >> s);
}

VB_BIG v128 vb_3int_slow(Ctx *c, int q, int size, int u, int opc, v128 vn, v128 vm, v128 vd) {
    vb_u n = {vn}, m = {vm}, d = {vd}, r = {{0, 0}};
    int lanes = (8 << q) >> size, eb = 8 << size;
    u64 mk = vb_mask(eb);
    int sg = (opc >= 0x10 && opc <= 0x13) || opc == 0x16 || opc == 0x17 ? 0 : u;   /* operands unsigned? */
    for (int i = 0; i < lanes; i++) {
        s128 a, b, x = 0;
        if (opc == 0x14 || opc == 0x15 || opc == 0x17) {
            int j = 2 * i, k = 2 * i + 1;
            a = j < lanes ? vb_getx(&n, size, j, sg) : vb_getx(&m, size, j - lanes, sg);
            b = k < lanes ? vb_getx(&n, size, k, sg) : vb_getx(&m, size, k - lanes, sg);
        } else {
            a = vb_getx(&n, size, i, sg);
            b = vb_getx(&m, size, i, sg);
        }
        s128 dl = (s128)vb_get(&d, size, i);
        switch (opc) {
        case 0x00: x = (a + b) >> 1; break;
        case 0x01: x = a + b; x = u ? vb_satU(c, x, eb) : vb_satS(c, x, eb); break;
        case 0x02: x = (a + b + 1) >> 1; break;
        case 0x04: x = (a - b) >> 1; break;
        case 0x05: x = a - b; x = u ? vb_satU(c, x, eb) : vb_satS(c, x, eb); break;
        case 0x06: x = a > b ? -1 : 0; break;
        case 0x07: x = a >= b ? -1 : 0; break;
        case 0x08: case 0x09: case 0x0A: case 0x0B:
            x = vb_shl_lane(c, a, (s8)vb_get(&m, size, i), eb, (opc >> 1) & 1, opc & 1 ? (opc == 0x09 || opc == 0x0B) : 0, u);
            break;
        case 0x0C: case 0x14: x = a > b ? a : b; break;
        case 0x0D: case 0x15: x = a < b ? a : b; break;
        case 0x0E: x = a > b ? a - b : b - a; break;
        case 0x0F: x = (s128)vb_get(&d, size, i) + (a > b ? a - b : b - a); break;
        case 0x10: x = u ? a - b : a + b; break;
        case 0x11: x = u ? (a == b ? -1 : 0) : (((u64)a & (u64)b & mk) != 0 ? -1 : 0); break;
        case 0x12: x = u ? dl - a * b : dl + a * b; break;
        case 0x13:
            if (u) { u32 acc = 0; for (int k = 0; k < 8; k++) if (((u8)b >> k) & 1) acc ^= (u32)(u8)a << k; x = acc; }
            else x = a * b;
            break;
        case 0x16: {
            s128 p = 2 * a * b;
            if (u) p += (s128)1 << (eb - 1);
            x = vb_satS(c, p >> eb, eb);
            break;
        }
        case 0x17: x = a + b; break;
        }
        vb_put(&r, size, i, (u64)x & mk);
    }
    return r.v;
}

VB_INL v128 vb_3int(Ctx *c, int q, int size, int u, int opc, v128 n, v128 m, v128 d) {
    v128 r;
    switch (opc) {
    case 0x06: r = vb_f_gt(size, u, n, m); break;
    case 0x07: r = vb_f_ge(size, u, n, m); break;
    case 0x0C: r = vb_f_max(size, u, n, m); break;
    case 0x0D: r = vb_f_min(size, u, n, m); break;
    case 0x0E: r = vb_f_abd(size, u, n, m); break;
    case 0x0F: r = vb_f_add(size, 1, d, vb_f_abd(size, u, n, m)); break;
    case 0x10: r = u ? vb_f_sub(size, 1, n, m) : vb_f_add(size, 1, n, m); break;
    case 0x11: r = u ? vb_f_eq(size, 1, n, m) : vb_f_tst(size, 1, n, m); break;
    case 0x12: if (size == 3) return vb_3int_slow(c, q, size, u, opc, n, m, d);
               r = u ? vb_f_sub(size, 1, d, vb_f_mul(size, 1, n, m)) : vb_f_add(size, 1, d, vb_f_mul(size, 1, n, m)); break;
    case 0x13: if (u || size == 3) return vb_3int_slow(c, q, size, u, opc, n, m, d);
               r = vb_f_mul(size, 1, n, m); break;
    case 0x08: if (size == 2) { r = vb_ushl32(u, n, m); break; }
               return vb_3int_slow(c, q, size, u, opc, n, m, d);
    default: return vb_3int_slow(c, q, size, u, opc, n, m, d);
    }
    return vb_q(q, r);
}

/* ---------------------------------------------------------------- three-different (long, wide, narrow) */
VB_BIG v128 vb_3diff(Ctx *c, int q, int size, int u, int opc, v128 vn, v128 vm, v128 vd) {
    vb_u n = {vn}, m = {vm}, d = {vd}, r = {{0, 0}};
    int es = size, ew = size + 1, eb = 8 << size, ebw = 16 << size;
    int cnt = 8 >> size;                       /* narrow elements per 64 bits = wide elements per 128 bits */
    u64 mkw = vb_mask(ebw), mk = vb_mask(eb);
    int off = q ? cnt : 0;
    if (opc == 4 || opc == 6) {                /* ADDHN/RADDHN/SUBHN/RSUBHN: wide inputs, narrow high half */
        if (q) r.d[0] = d.d[0];
        for (int i = 0; i < cnt; i++) {
            s128 a = (s128)vb_get(&n, ew, i), b = (s128)vb_get(&m, ew, i);
            s128 x = opc == 4 ? a + b : a - b;
            if (u) x += (s128)1 << (eb - 1);
            vb_put(&r, es, i + off, (u64)(x >> eb) & mk);
        }
        return r.v;
    }
    if (opc == 14) {                           /* PMULL 8x8 -> 16 */
        for (int i = 0; i < 8; i++) {
            u32 a = vb_get(&n, 0, i + (q ? 8 : 0)), b = vb_get(&m, 0, i + (q ? 8 : 0)), acc = 0;
            for (int k = 0; k < 8; k++) if ((b >> k) & 1) acc ^= a << k;
            r.h[i] = (u16)acc;
        }
        return r.v;
    }
    for (int i = 0; i < cnt; i++) {
        s128 a, b, x = 0, dl = (s128)vb_get(&d, ew, i);
        s128 dls = vb_gets(&d, ew, i);
        if (opc == 1 || opc == 3) a = vb_getx(&n, ew, i, u);
        else a = vb_getx(&n, es, i + off, u);
        b = vb_getx(&m, es, i + off, u);
        switch (opc) {
        case 0: case 1: x = a + b; break;
        case 2: case 3: x = a - b; break;
        case 5: x = dl + (a > b ? a - b : b - a); break;
        case 7: x = a > b ? a - b : b - a; break;
        case 8: x = dl + a * b; break;
        case 10: x = dl - a * b; break;
        case 12: x = a * b; break;
        case 9: case 11: case 13: {
            s128 p = vb_satS(c, 2 * a * b, ebw);
            if (opc == 13) x = p;
            else x = vb_satS(c, opc == 9 ? dls + p : dls - p, ebw);
            break;
        }
        }
        vb_put(&r, ew, i, (u64)x & mkw);
    }
    return r.v;
}

/* ---------------------------------------------------------------- two-register miscellaneous, integer */
VB_INL int vb_clz(u64 v, int eb) { int n = 0; for (int i = eb - 1; i >= 0 && !((v >> i) & 1); i--) n++; return n; }

VB_BIG v128 vb_2misc(Ctx *c, int q, int size, int u, int opc, v128 vn, v128 vd) {
    vb_u n = {vn}, d = {vd}, r = {{0, 0}};
    int rbit = u && opc == 5 && size == 1;       /* RBIT works on bytes */
    if (rbit) size = 0;
    int lanes = (8 << q) >> size, eb = 8 << size;
    u64 mk = vb_mask(eb);
    switch (opc) {
    case 0: case 1: {                          /* REV64 / REV32 / REV16 */
        int cb = opc == 1 ? 2 : (u ? 4 : 8);
        int per = cb >> size;
        for (int i = 0; i < lanes; i++) vb_put(&r, size, (i / per) * per + (per - 1 - i % per), vb_get(&n, size, i));
        return r.v;
    }
    case 2: case 6: {                          /* SADDLP/UADDLP/SADALP/UADALP */
        int ew = size + 1, outl = lanes / 2;
        u64 mkw = vb_mask(16 << size);
        for (int i = 0; i < outl; i++) {
            s128 x = vb_getx(&n, size, 2 * i, u) + vb_getx(&n, size, 2 * i + 1, u);
            if (opc == 6) x += (s128)vb_get(&d, ew, i);
            vb_put(&r, ew, i, (u64)x & mkw);
        }
        return r.v;
    }
    case 18: case 20: {                        /* XTN / SQXTUN / SQXTN / UQXTN */
        int cnt = 8 >> size, ew = size + 1, off = q ? cnt : 0;
        if (q) r.d[0] = d.d[0];
        for (int i = 0; i < cnt; i++) {
            s128 x;
            if (opc == 18 && !u) x = (s128)vb_get(&n, ew, i);
            else if (opc == 18) x = vb_satU(c, vb_gets(&n, ew, i), eb);
            else if (u) x = vb_satU(c, (s128)vb_get(&n, ew, i), eb);
            else x = vb_satS(c, vb_gets(&n, ew, i), eb);
            vb_put(&r, size, i + off, (u64)x & mk);
        }
        return r.v;
    }
    case 19: {                                 /* SHLL: shift by element size */
        int cnt = 8 >> size, ew = size + 1, off = q ? cnt : 0;
        for (int i = 0; i < cnt; i++) vb_put(&r, ew, i, vb_get(&n, size, i + off) << eb);
        return r.v;
    }
    }
    for (int i = 0; i < lanes; i++) {
        s128 a = vb_gets(&n, size, i), x = 0;
        u64 ua = vb_get(&n, size, i);
        switch (opc) {
        case 3:                                /* SUQADD / USQADD */
            if (!u) x = vb_satS(c, vb_gets(&d, size, i) + (s128)ua, eb);
            else x = vb_satU(c, (s128)vb_get(&d, size, i) + a, eb);
            break;
        case 4:                                /* CLS / CLZ */
            if (u) x = vb_clz(ua, eb);
            else { u64 sb = (ua >> (eb - 1)) & 1; int k = 0; for (int j = eb - 2; j >= 0 && (((ua >> j) & 1) == sb); j--) k++; x = k; }
            break;
        case 5:
            if (!u) x = __builtin_popcountll(ua);
            else if (!rbit) x = ~ua;
            else { u32 t = 0; for (int j = 0; j < 8; j++) if ((ua >> j) & 1) t |= 1u << (7 - j); x = t; }
            break;
        case 7: {                              /* SQABS / SQNEG */
            s128 v = u ? -a : (a < 0 ? -a : a);
            x = vb_satS(c, v, eb);
            break;
        }
        case 8: x = u ? (a >= 0 ? -1 : 0) : (a > 0 ? -1 : 0); break;
        case 9: x = u ? (a <= 0 ? -1 : 0) : (a == 0 ? -1 : 0); break;
        case 10: x = a < 0 ? -1 : 0; break;
        case 11: x = u ? -a : (a < 0 ? -a : a); break;
        }
        vb_put(&r, size, i, (u64)x & mk);
    }
    return r.v;
}
VB_INL v128 vb_2misc_f(Ctx *c, int q, int size, int u, int opc, v128 n, v128 d) {
    if (opc == 5 && u && size == 0) return vb_q(q, ~n);
    return vb_2misc(c, q, size, u, opc, n, d);
}

/* ---------------------------------------------------------------- across lanes (integer) */
VB_BIG v128 vb_across(Ctx *c, int q, int size, int u, int opc, v128 vn) {
    vb_u n = {vn}, r = {{0, 0}};
    int lanes = (8 << q) >> size;
    int eb = 8 << size;
    s128 acc = 0;
    switch (opc) {
    case 0x03:                                 /* SADDLV / UADDLV */
        for (int i = 0; i < lanes; i++) acc += vb_getx(&n, size, i, u);
        vb_put(&r, size + 1, 0, (u64)acc & vb_mask(eb * 2));
        return r.v;
    case 0x0A: case 0x1A:
        acc = vb_getx(&n, size, 0, u);
        for (int i = 1; i < lanes; i++) {
            s128 v = vb_getx(&n, size, i, u);
            if (opc == 0x0A ? v > acc : v < acc) acc = v;
        }
        break;
    case 0x1B:
        for (int i = 0; i < lanes; i++) acc += (s128)vb_get(&n, size, i);
        break;
    }
    vb_put(&r, size, 0, (u64)acc & vb_mask(eb));
    return r.v;
}

/* ---------------------------------------------------------------- permute, extract */
VB_INL v128 vb_perm(int q, int size, int opc, v128 vn, v128 vm) {
    vb_u n = {vn}, m = {vm}, r = {{0, 0}};
    int lanes = (8 << q) >> size, half = lanes / 2;
    int part = opc >> 2;                       /* 0 for UZP1/TRN1/ZIP1, 1 for the "2" forms */
    int kind = opc & 3;                        /* 1 UZP, 2 TRN, 3 ZIP */
    for (int i = 0; i < lanes; i++) {
        u64 v;
        if (kind == 1) {
            int j = 2 * (i < half ? i : i - half) + part;
            v = i < half ? vb_get(&n, size, j) : vb_get(&m, size, j);
        } else if (kind == 2) {
            int j = (i & ~1) + part;
            v = (i & 1) ? vb_get(&m, size, j) : vb_get(&n, size, j);
        } else {
            int j = (i >> 1) + part * half;
            v = (i & 1) ? vb_get(&m, size, j) : vb_get(&n, size, j);
        }
        vb_put(&r, size, i, v);
    }
    return r.v;
}
VB_INL v128 vb_ext(int q, int imm4, v128 n, v128 m) {
    u8 t[32];
    v128 r;
    if (q) { memcpy(t, &n, 16); memcpy(t + 16, &m, 16); memcpy(&r, t + imm4, 16); return r; }
    memcpy(t, &n, 8); memcpy(t + 8, &m, 8);
    r = vb_zero();
    memcpy(&r, t + imm4, 8);
    return r;
}

/* ---------------------------------------------------------------- copy (DUP, INS, UMOV, SMOV) */
VB_INL v128 vb_dup(int q, int esz, u64 v) {
    vb_u r = {{0, 0}};
    int lanes = (8 << q) >> esz;
    for (int i = 0; i < lanes; i++) vb_put(&r, esz, i, v);
    return r.v;
}
VB_INL v128 vb_dupe(int q, int esz, int idx, v128 n) { vb_u x = {n}; return vb_dup(q, esz, vb_get(&x, esz, idx)); }
VB_INL v128 vb_ins(int esz, int idx, v128 d, u64 v) { vb_u r = {d}; vb_put(&r, esz, idx, v); return r.v; }
VB_INL v128 vb_inse(int esz, int idx, int idx2, v128 d, v128 n) { vb_u x = {n}; return vb_ins(esz, idx, d, vb_get(&x, esz, idx2)); }
VB_INL u64 vb_umov(int q, int esz, int sgn, int idx, v128 n) {
    vb_u x = {n};
    if (!sgn) return vb_get(&x, esz, idx);
    s64 v = (s64)vb_gets(&x, esz, idx);
    return q ? (u64)v : (u32)v;
}

/* ---------------------------------------------------------------- shifts by immediate */
VB_INL v128 vb_shr_fast(int q, int esz, int u, int sh, v128 n) {
    v128 r;
    if (sh >= (8 << esz)) { if (u) return vb_zero(); sh = (8 << esz) - 1; }
    switch (esz) {
    case 0: r = u ? (v128)((vb_u8x16)n >> sh) : (v128)((vb_s8x16)n >> sh); break;
    case 1: r = u ? (v128)((vb_u16x8)n >> sh) : (v128)((vb_s16x8)n >> sh); break;
    case 2: r = u ? (v128)((vb_u32x4)n >> sh) : (v128)((vb_s32x4)n >> sh); break;
    default: r = u ? (v128)((vb_u64x2)n >> sh) : (v128)((vb_s64x2)n >> sh); break;
    }
    return r;
}
VB_INL v128 vb_shl_fast(int esz, int sh, v128 n) {
    switch (esz) {
    case 0: return (v128)((vb_u8x16)n << sh);
    case 1: return (v128)((vb_u16x8)n << sh);
    case 2: return (v128)((vb_u32x4)n << sh);
    default: return (v128)((vb_u64x2)n << sh);
    }
}
VB_BIG v128 vb_shift_slow(Ctx *c, int q, int esz, int sh, int opc, int u, v128 vn, v128 vd) {
    vb_u n = {vn}, d = {vd}, r = {{0, 0}};
    int lanes = (8 << q) >> esz, eb = 8 << esz;
    u64 mk = vb_mask(eb);
    for (int i = 0; i < lanes; i++) {
        s128 a = vb_getx(&n, esz, i, u && opc != 6 && opc != 7 ? 1 : (opc == 7 ? u : 0)), x = 0;
        u64 dl = vb_get(&d, esz, i);
        switch (opc) {
        case 0: x = a >> sh; break;
        case 1: x = (s128)dl + (a >> sh); break;
        case 2: x = (a + ((s128)1 << (sh - 1))) >> sh; break;
        case 3: x = (s128)dl + ((a + ((s128)1 << (sh - 1))) >> sh); break;
        case 4: { u64 keep = sh >= eb ? 0 : (mk >> sh); x = (dl & ~keep & mk) | (((u64)vb_get(&n, esz, i) >> (sh >= eb ? 63 : sh)) & (sh >= eb ? 0 : keep)); break; }
        case 5:
            if (u) { u64 keep = (mk << sh) & mk; x = (dl & ~keep & mk) | ((vb_get(&n, esz, i) << sh) & keep); }
            else x = sh >= eb ? 0 : (s128)((u128)a << sh);
            break;
        case 6: x = vb_satU(c, (s128)((u128)a << sh), eb); break;
        case 7: x = u ? vb_satU(c, (s128)((u128)a << sh), eb) : vb_satS(c, (s128)((u128)a << sh), eb); break;
        }
        vb_put(&r, esz, i, (u64)x & mk);
    }
    return r.v;
}
VB_INL v128 vb_shift(Ctx *c, int q, int esz, int sh, int opc, int u, v128 n, v128 d) {
    switch (opc) {
    case 0: return vb_q(q, vb_shr_fast(q, esz, u, sh, n));
    case 1: return vb_q(q, vb_f_add(esz, 1, d, vb_shr_fast(q, esz, u, sh, n)));
    case 5: if (!u) return vb_q(q, vb_shl_fast(esz, sh, n)); /* fallthrough */
    default: return vb_shift_slow(c, q, esz, sh, opc, u, n, d);
    }
}
VB_BIG v128 vb_shn(Ctx *c, int q, int esz, int sh, int kind, v128 vn, v128 vd) {
    vb_u n = {vn}, d = {vd}, r = {{0, 0}};
    int cnt = 8 >> esz, ew = esz + 1, eb = 8 << esz, off = q ? cnt : 0;
    u64 mk = vb_mask(eb);
    if (q) r.d[0] = d.d[0];
    for (int i = 0; i < cnt; i++) {
        int rnd = kind & 1;
        int uns_src = kind >= 6;
        s128 a = vb_getx(&n, ew, i, uns_src);
        s128 v = rnd ? (a + ((s128)1 << (sh - 1))) >> sh : a >> sh;
        s128 x;
        if (kind < 2) x = v;
        else if (kind < 4) x = vb_satU(c, v, eb);
        else if (kind < 6) x = vb_satS(c, v, eb);
        else x = vb_satU(c, v, eb);
        vb_put(&r, esz, i + off, (u64)x & mk);
    }
    return r.v;
}
VB_INL v128 vb_shll(int q, int esz, int sh, int u, v128 vn) {
    vb_u n = {vn}, r = {{0, 0}};
    int cnt = 8 >> esz, ew = esz + 1, off = q ? cnt : 0;
    u64 mkw = vb_mask(16 << esz);
    for (int i = 0; i < cnt; i++) vb_put(&r, ew, i, (u64)(vb_getx(&n, esz, i + off, u) << sh) & mkw);
    return r.v;
}

/* ---------------------------------------------------------------- table lookup */
VB_INL v128 vb_tbl(int q, int len, int op, v128 d, v128 t0, v128 t1, v128 t2, v128 t3, v128 idx) {
    __m128i ix = (__m128i)idx, zero = _mm_setzero_si128();
    __m128i acc = zero, any = zero;
    v128 tab[4] = {t0, t1, t2, t3};
    for (int j = 0; j <= len; j++) {
        __m128i ij = _mm_sub_epi8(ix, _mm_set1_epi8((char)(16 * j)));
        __m128i ok = _mm_cmpeq_epi8(_mm_subs_epu8(ij, _mm_set1_epi8(15)), zero);
        acc = _mm_or_si128(acc, _mm_and_si128(_mm_shuffle_epi8((__m128i)tab[j], ij), ok));
        any = _mm_or_si128(any, ok);
    }
    __m128i r = op ? _mm_or_si128(acc, _mm_andnot_si128(any, (__m128i)d)) : acc;
    return vb_q(q, (v128)r);
}

/* ---------------------------------------------------------------- structure loads and stores */
VB_INL int vb_nregs(int opc) { return opc == 0 ? 4 : opc == 2 ? 4 : opc == 4 ? 3 : opc == 6 ? 3 : opc == 7 ? 1 : opc == 8 ? 2 : 2; }
VB_INL int vb_selem(int opc) { return opc == 0 ? 4 : opc == 4 ? 3 : opc == 8 ? 2 : 1; }
VB_BIG void vb_ldn_slow(v128 *t, u64 a, int q, int esz, int selem) {
    vb_u r[4] = {{{0, 0}}, {{0, 0}}, {{0, 0}}, {{0, 0}}};
    int el = (8 << q) >> esz;
    for (int e = 0; e < el; e++)
        for (int s = 0; s < selem; s++) vb_put(&r[s], esz, e, ir_ld(1 << esz, 0, 1, a + (u64)((e * selem + s) << esz)));
    for (int s = 0; s < selem; s++) t[s] = r[s].v;
}
VB_BIG void vb_stn_slow(const v128 *t, u64 a, int q, int esz, int selem) {
    int el = (8 << q) >> esz;
    for (int e = 0; e < el; e++)
        for (int s = 0; s < selem; s++) { vb_u x = {t[s]}; ir_st(1 << esz, a + (u64)((e * selem + s) << esz), vb_get(&x, esz, e)); }
}
VB_INL void vb_ldm(v128 *t, u64 a, int q, int esz, int opc) {
    int sel = vb_selem(opc), nr = vb_nregs(opc), ds = 8 << q;
    if (sel == 1) { for (int r = 0; r < nr; r++) t[r] = ir_ldv(ds, a + (u64)r * ds); }
    else vb_ldn_slow(t, a, q, esz, sel);
}
VB_INL void vb_stm(const v128 *t, u64 a, int q, int esz, int opc) {
    int sel = vb_selem(opc), nr = vb_nregs(opc), ds = 8 << q;
    if (sel == 1) { for (int r = 0; r < nr; r++) ir_stv(ds, a + (u64)r * ds, t[r]); }
    else vb_stn_slow(t, a, q, esz, sel);
}
VB_INL void vb_ldl(v128 *t, u64 a, int esz, int sel, int idx) {
    for (int s = 0; s < sel; s++) t[s] = vb_ins(esz, idx, t[s], ir_ld(1 << esz, 0, 1, a + (u64)(s << esz)));
}
VB_INL void vb_stl(const v128 *t, u64 a, int esz, int sel, int idx) {
    for (int s = 0; s < sel; s++) { vb_u x = {t[s]}; ir_st(1 << esz, a + (u64)(s << esz), vb_get(&x, esz, idx)); }
}
VB_INL void vb_ldr(v128 *t, u64 a, int q, int esz, int sel) {
    for (int s = 0; s < sel; s++) t[s] = vb_dup(q, esz, ir_ld(1 << esz, 0, 1, a + (u64)(s << esz)));
}


/* ================================================================ floating point (vector)
 * Scalar FP engine: exact Arm semantics (FPCR.FZ/DN/RMode, NaN rules, cumulative flags) using host float/double
 * arithmetic under a controlled MXCSR (round mode and flags). Vector fast paths for the common FPCR=0, no-NaN
 * case run SSE/FMA directly; everything else goes through the scalar engine lane by lane. */
typedef struct { u32 fpcr, fl; } vb_env;
#define VB_IOC 0x01u
#define VB_DZC 0x02u
#define VB_OFC 0x04u
#define VB_UFC 0x08u
#define VB_IXC 0x10u
#define VB_IDC 0x80u
#define VB_FZ(e) (((e)->fpcr >> 24) & 1)
#define VB_DNB(e) (((e)->fpcr >> 25) & 1)
#define VB_SGN(w) ((w) == 32 ? 0x80000000ull : 0x8000000000000000ull)
#define VB_EXP(w) ((w) == 32 ? 0x7F800000ull : 0x7FF0000000000000ull)
#define VB_FRC(w) ((w) == 32 ? 0x007FFFFFull : 0x000FFFFFFFFFFFFFull)
#define VB_QB(w) ((w) == 32 ? 0x00400000ull : 0x0008000000000000ull)
#define VB_DNAN(w) (VB_EXP(w) | VB_QB(w))
#define VB_ONE(w) ((w) == 32 ? 0x3F800000ull : 0x3FF0000000000000ull)
#define VB_TWO(w) ((w) == 32 ? 0x40000000ull : 0x4000000000000000ull)
#define VB_THREE(w) ((w) == 32 ? 0x40400000ull : 0x4008000000000000ull)
#define VB_ONEHALF(w) ((w) == 32 ? 0x3FC00000ull : 0x3FF8000000000000ull)

static const u32 vb_mx_default = 0x1F80u;
static const u32 vb_rctab[4] = {0x1F80u, 0x1F80u | (2u << 13), 0x1F80u | (1u << 13), 0x1F80u | (3u << 13)};
VB_INL void vb_hbegin(u32 fpcr) { u32 m = vb_rctab[(fpcr >> 22) & 3]; __asm__ volatile("ldmxcsr %0" ::"m"(m) : "memory"); }
VB_INL u32 vb_hflags(void) { u32 m; __asm__ volatile("stmxcsr %0" : "=m"(m)::"memory"); return (m & 1) | ((m >> 1) & 0x1E); }
VB_INL u32 vb_hend(void) { u32 f = vb_hflags(); __asm__ volatile("ldmxcsr %0" ::"m"(vb_mx_default) : "memory"); return f; }

VB_INL int vb_isnan(int w, u64 b) { return (b & ~VB_SGN(w)) > VB_EXP(w); }
VB_INL int vb_issnan(int w, u64 b) { return vb_isnan(w, b) && !(b & VB_QB(w)); }
VB_INL int vb_isinf(int w, u64 b) { return (b & ~VB_SGN(w)) == VB_EXP(w); }
VB_INL int vb_iszero(int w, u64 b) { return (b & ~VB_SGN(w)) == 0; }
VB_INL int vb_isden(int w, u64 b) { return (b & VB_EXP(w)) == 0 && (b & VB_FRC(w)) != 0; }

VB_INL u64 vb_fl_in(vb_env *e, int w, u64 b) {
    if (VB_FZ(e) && vb_isden(w, b)) { e->fl |= VB_IDC; return b & VB_SGN(w); }
    return b;
}
VB_INL u64 vb_qnan(vb_env *e, int w, u64 b) {
    if (vb_issnan(w, b)) e->fl |= VB_IOC;
    return VB_DNB(e) ? VB_DNAN(w) : (b | VB_QB(w));
}
VB_INL u64 vb_nan2(vb_env *e, int w, u64 a, u64 b) {
    if (vb_issnan(w, a)) return vb_qnan(e, w, a);
    if (vb_issnan(w, b)) return vb_qnan(e, w, b);
    return vb_isnan(w, a) ? vb_qnan(e, w, a) : vb_qnan(e, w, b);
}
VB_INL u64 vb_nan3(vb_env *e, int w, u64 a, u64 b, u64 c3) {
    if (vb_issnan(w, a)) return vb_qnan(e, w, a);
    if (vb_issnan(w, b)) return vb_qnan(e, w, b);
    if (vb_issnan(w, c3)) return vb_qnan(e, w, c3);
    if (vb_isnan(w, a)) return vb_qnan(e, w, a);
    return vb_isnan(w, b) ? vb_qnan(e, w, b) : vb_qnan(e, w, c3);
}
/* result post-processing after a host operation: default NaN for invalid results, flush-to-zero of tiny results */
/* x86 detects tininess after rounding, Arm before rounding: a result that is exactly the smallest normal after a
 * rounding step may have been tiny before it. Re-run the operation truncating toward zero to find out. */
static u64 vb_redo_rz(int w, int op, u64 a, u64 b, u64 c3);
VB_INL u64 vb_fin2(vb_env *e, int w, u64 r, u32 fl, int op, u64 a, u64 b, u64 c3) {
    if ((fl & VB_IXC) && !(fl & VB_UFC) && (r & ~VB_SGN(w)) == (w == 32 ? 0x00800000ull : 0x0010000000000000ull) && op >= 0) {
        u64 z = vb_redo_rz(w, op, a, b, c3);
        if ((z & ~VB_SGN(w)) < (w == 32 ? 0x00800000ull : 0x0010000000000000ull)) fl |= VB_UFC;
    }
    if (vb_isnan(w, r)) r = VB_DNAN(w);
    if (VB_FZ(e) && (vb_isden(w, r) || (fl & VB_UFC))) { r &= VB_SGN(w); fl = (fl & ~(VB_IXC | VB_UFC)) | VB_UFC; }
    e->fl |= fl;
    return r;
}
VB_INL u64 vb_fin(vb_env *e, int w, u64 r, u32 fl) { return vb_fin2(e, w, r, fl, -1, 0, 0, 0); }

VB_INL u64 vb_host2(int w, int op, u64 a, u64 b) {
    if (w == 32) {
        float x = __builtin_bit_cast(float, (u32)a), y = __builtin_bit_cast(float, (u32)b), r;
        __asm__ volatile("" : "+x"(x), "+x"(y));
        switch (op) {
        case 0: r = x + y; break;
        case 1: r = x - y; break;
        case 2: r = x * y; break;
        case 3: r = x / y; break;
        default: r = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); break;
        }
        __asm__ volatile("" : "+x"(r));
        return __builtin_bit_cast(u32, r);
    } else {
        double x = __builtin_bit_cast(double, a), y = __builtin_bit_cast(double, b), r;
        __asm__ volatile("" : "+x"(x), "+x"(y));
        switch (op) {
        case 0: r = x + y; break;
        case 1: r = x - y; break;
        case 2: r = x * y; break;
        case 3: r = x / y; break;
        default: r = _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x))); break;
        }
        __asm__ volatile("" : "+x"(r));
        return __builtin_bit_cast(u64, r);
    }
}
/* fused: acc + x*y */
VB_INL u64 vb_host3(int w, u64 acc, u64 x, u64 y) {
    if (w == 32) {
        __m128 a = _mm_set_ss(__builtin_bit_cast(float, (u32)acc)), b = _mm_set_ss(__builtin_bit_cast(float, (u32)x)),
               d = _mm_set_ss(__builtin_bit_cast(float, (u32)y));
        __asm__ volatile("" : "+x"(a), "+x"(b), "+x"(d));
        __m128 r = _mm_fmadd_ss(b, d, a);
        __asm__ volatile("" : "+x"(r));
        return __builtin_bit_cast(u32, _mm_cvtss_f32(r));
    } else {
        __m128d a = _mm_set_sd(__builtin_bit_cast(double, acc)), b = _mm_set_sd(__builtin_bit_cast(double, x)),
                d = _mm_set_sd(__builtin_bit_cast(double, y));
        __asm__ volatile("" : "+x"(a), "+x"(b), "+x"(d));
        __m128d r = _mm_fmadd_sd(b, d, a);
        __asm__ volatile("" : "+x"(r));
        return __builtin_bit_cast(u64, _mm_cvtsd_f64(r));
    }
}

static u64 vb_redo_rz(int w, int op, u64 a, u64 b, u64 c3) {
    vb_hbegin(3u << 22);
    u64 r;
    if (op < 5) r = vb_host2(w, op, a, b);
    else if (op == 5) r = vb_host3(w, a, b, c3);
    else {
        double x = __builtin_bit_cast(double, a);
        __asm__ volatile("" : "+x"(x));
        float f = (float)x;
        __asm__ volatile("" : "+x"(f));
        r = __builtin_bit_cast(u32, f);
    }
    (void)vb_hend();
    return r;
}
static u64 vb_fbin(vb_env *e, int w, int op, u64 a, u64 b) {      /* 0 add 1 sub 2 mul 3 div */
    a = vb_fl_in(e, w, a);
    b = vb_fl_in(e, w, b);
    if (vb_isnan(w, a) || vb_isnan(w, b)) return vb_nan2(e, w, a, b);
    vb_hbegin(e->fpcr);
    u64 r = vb_host2(w, op, a, b);
    return vb_fin2(e, w, r, vb_hend(), op, a, b, 0);
}
static u64 vb_fsqrt(vb_env *e, int w, u64 a) {
    a = vb_fl_in(e, w, a);
    if (vb_isnan(w, a)) return vb_qnan(e, w, a);
    vb_hbegin(e->fpcr);
    u64 r = vb_host2(w, 4, a, a);
    return vb_fin2(e, w, r, vb_hend(), 4, a, a, 0);
}
static u64 vb_fmulx(vb_env *e, int w, u64 a, u64 b) {
    a = vb_fl_in(e, w, a);
    b = vb_fl_in(e, w, b);
    if (vb_isnan(w, a) || vb_isnan(w, b)) return vb_nan2(e, w, a, b);
    if ((vb_isinf(w, a) && vb_iszero(w, b)) || (vb_iszero(w, a) && vb_isinf(w, b))) return VB_TWO(w) | ((a ^ b) & VB_SGN(w));
    vb_hbegin(e->fpcr);
    u64 r = vb_host2(w, 2, a, b);
    return vb_fin2(e, w, r, vb_hend(), 2, a, b, 0);
}
static u64 vb_ffma(vb_env *e, int w, u64 acc, u64 x, u64 y) {     /* acc + x*y, one rounding */
    acc = vb_fl_in(e, w, acc);
    x = vb_fl_in(e, w, x);
    y = vb_fl_in(e, w, y);
    int infz = (vb_isinf(w, x) && vb_iszero(w, y)) || (vb_iszero(w, x) && vb_isinf(w, y));
    if (vb_isnan(w, acc) && !vb_issnan(w, acc) && infz) { e->fl |= VB_IOC; return VB_DNAN(w); }
    if (vb_isnan(w, acc) || vb_isnan(w, x) || vb_isnan(w, y)) return vb_nan3(e, w, acc, x, y);
    vb_hbegin(e->fpcr);
    u64 r = vb_host3(w, acc, x, y);
    return vb_fin2(e, w, r, vb_hend(), 5, acc, x, y);
}
static u64 vb_frecps(vb_env *e, int w, u64 a, u64 b) {
    a ^= VB_SGN(w);
    a = vb_fl_in(e, w, a);
    b = vb_fl_in(e, w, b);
    if (vb_isnan(w, a) || vb_isnan(w, b)) return vb_nan2(e, w, a, b);
    if ((vb_isinf(w, a) && vb_iszero(w, b)) || (vb_iszero(w, a) && vb_isinf(w, b))) return VB_TWO(w);
    vb_hbegin(e->fpcr);
    u64 r = vb_host3(w, VB_TWO(w), a, b);
    return vb_fin2(e, w, r, vb_hend(), 5, VB_TWO(w), a, b);
}
static u64 vb_frsqrts(vb_env *e, int w, u64 a, u64 b) {
    a ^= VB_SGN(w);
    a = vb_fl_in(e, w, a);
    b = vb_fl_in(e, w, b);
    if (vb_isnan(w, a) || vb_isnan(w, b)) return vb_nan2(e, w, a, b);
    if ((vb_isinf(w, a) && vb_iszero(w, b)) || (vb_iszero(w, a) && vb_isinf(w, b))) return VB_ONEHALF(w);
    vb_hbegin(e->fpcr);
    u64 r;
    u32 ha = (u32)a, hb = (u32)b;
    int half32 = w == 32 && (ha & 0x7FFFFFFFu) > 0x00FFFFFFu && (hb & 0x7FFFFFFFu) > 0x00FFFFFFu &&
                 ((ha >> 23) & 0xFF) != 0xFF && ((hb >> 23) & 0xFF) != 0xFF;
    if (half32) {       /* (3 + a*b) / 2 = 1.5 + (a/2)*b with one rounding: halve the larger operand exactly */
        u32 *q = (ha & 0x7FFFFFFFu) >= (hb & 0x7FFFFFFFu) ? &ha : &hb;
        *q -= 1u << 23;
        r = vb_host3(32, VB_ONEHALF(32), ha, hb);
    } else if (w == 32) {      /* subnormal or tiny operands: evaluate in double, then one rounding to single */
        __m128d xa = _mm_set_sd(__builtin_bit_cast(float, (u32)a)), ya = _mm_set_sd(__builtin_bit_cast(float, (u32)b)), three = _mm_set_sd(3.0);
        __asm__ volatile("" : "+x"(xa), "+x"(ya), "+x"(three));
        __m128d t = _mm_fmadd_sd(xa, ya, three);
        double h = _mm_cvtsd_f64(t) * 0.5;
        __asm__ volatile("" : "+x"(h));
        float f = (float)h;
        __asm__ volatile("" : "+x"(f));
        r = __builtin_bit_cast(u32, f);
    } else {
        u64 xa = a, yb = b;       /* (3 + a*b) / 2 = 1.5 + (a/2)*b with one rounding: halve the larger operand exactly */
        u64 *p = (xa & ~VB_SGN(64)) >= (yb & ~VB_SGN(64)) ? &xa : &yb;
        if (((*p >> 52) & 0x7FF) > 1 && ((xa >> 52) & 0x7FF) != 0x7FF && ((yb >> 52) & 0x7FF) != 0x7FF) {
            *p -= 1ull << 52;
            r = vb_host3(64, VB_ONEHALF(64), xa, yb);
        } else {
            u64 t = vb_host3(64, VB_THREE(64), a, b);
            double h = __builtin_bit_cast(double, t) * 0.5;
            __asm__ volatile("" : "+x"(h));
            r = __builtin_bit_cast(u64, h);
        }
    }
    return vb_fin(e, w, r, vb_hend());
}

/* compare: kind 0 EQ 1 GE 2 GT; returns 0/1 */
static int vb_fcmp(vb_env *e, int w, int kind, u64 a, u64 b) {
    a = vb_fl_in(e, w, a);
    b = vb_fl_in(e, w, b);
    if (vb_isnan(w, a) || vb_isnan(w, b)) {
        if (kind != 0 || vb_issnan(w, a) || vb_issnan(w, b)) e->fl |= VB_IOC;
        return 0;
    }
    if (w == 32) {
        float x = __builtin_bit_cast(float, (u32)a), y = __builtin_bit_cast(float, (u32)b);
        return kind == 0 ? x == y : kind == 1 ? x >= y : x > y;
    }
    double x = __builtin_bit_cast(double, a), y = __builtin_bit_cast(double, b);
    return kind == 0 ? x == y : kind == 1 ? x >= y : x > y;
}
/* max/min; num = FMAXNM/FMINNM rules */
static u64 vb_fmaxmin(vb_env *e, int w, int ismin, int num, u64 a, u64 b) {
    a = vb_fl_in(e, w, a);
    b = vb_fl_in(e, w, b);
    if (num) {
        int qa = vb_isnan(w, a) && !vb_issnan(w, a), qb = vb_isnan(w, b) && !vb_issnan(w, b);
        u64 inf = VB_EXP(w) | (ismin ? 0 : VB_SGN(w));
        if (qa && !qb && !vb_isnan(w, b)) a = inf;
        else if (qa && vb_issnan(w, b)) a = inf;
        else if (qb && !qa && !vb_isnan(w, a)) b = inf;
        else if (qb && vb_issnan(w, a)) b = inf;
    }
    if (vb_isnan(w, a) || vb_isnan(w, b)) return vb_nan2(e, w, a, b);
    if (vb_iszero(w, a) && vb_iszero(w, b)) return ismin ? (a | b) : (a & b);
    int gt;
    if (w == 32) { float x = __builtin_bit_cast(float, (u32)a), y = __builtin_bit_cast(float, (u32)b); gt = x > y; }
    else { double x = __builtin_bit_cast(double, a), y = __builtin_bit_cast(double, b); gt = x > y; }
    int lt = !gt && a != b;
    return ismin ? (lt ? a : b) : (gt ? a : b);
}

/* estimates */
VB_INL u32 vb_recip_est9(u32 a) { a = a * 2 + 1; u32 b = (1u << 19) / a; return (b + 1) / 2; }
VB_INL u32 vb_rsqrt_est9(u32 a) {
    if (a < 256) a = a * 2 + 1; else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    u32 b = 512;
    while ((u64)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (b + 1) / 2;
}
static u64 vb_frecpe(vb_env *e, int w, u64 b) {
    b = vb_fl_in(e, w, b);
    u64 sgn = b & VB_SGN(w);
    if (vb_isnan(w, b)) return vb_qnan(e, w, b);
    if (vb_isinf(w, b)) return sgn;
    if (vb_iszero(w, b)) { e->fl |= VB_DZC; return sgn | VB_EXP(w); }
    int fb = w == 32 ? 23 : 52;
    u64 frac = b & VB_FRC(w);
    int ex = (int)((b >> fb) & (w == 32 ? 0xFF : 0x7FF));
    if (ex == 0 && (frac >> (fb - 2)) == 0) {            /* |value| < 2^-128 (single) / 2^-1024 (double) */
        int mode = (e->fpcr >> 22) & 3;
        int to_inf = mode == 0 || (mode == 1 && !sgn) || (mode == 2 && sgn);
        e->fl |= VB_OFC | VB_IXC;
        return sgn | (to_inf ? VB_EXP(w) : ((VB_EXP(w) - (1ull << fb)) | VB_FRC(w)));
    }
    if (VB_FZ(e) && ex >= (w == 32 ? 253 : 2045)) { e->fl |= VB_UFC; return sgn; }
    u64 m52 = (1ull << 52) - 1;
    u64 fr = frac << (52 - fb);
    if (ex == 0) {
        if (!((fr >> 51) & 1)) { ex = -1; fr = (fr << 2) & m52; } else fr = (fr << 1) & m52;
    }
    int rexp = (w == 32 ? 253 : 2045) - ex;
    u32 est = vb_recip_est9(256 + (u32)(fr >> 44));
    u64 f2 = (u64)(est & 0xFF) << 44;
    if (rexp == 0) f2 = (1ull << 51) | (f2 >> 1);
    else if (rexp == -1) { f2 = (1ull << 50) | (f2 >> 2); rexp = 0; }
    return w == 32 ? (sgn | ((u64)(rexp & 0xFF) << 23) | (f2 >> 29)) : (sgn | ((u64)(rexp & 0x7FF) << 52) | f2);
}
static u64 vb_frsqrte(vb_env *e, int w, u64 b) {
    b = vb_fl_in(e, w, b);
    u64 sgn = b & VB_SGN(w);
    if (vb_isnan(w, b)) return vb_qnan(e, w, b);
    if (vb_iszero(w, b)) { e->fl |= VB_DZC; return sgn | VB_EXP(w); }
    if (sgn) { e->fl |= VB_IOC; return VB_DNAN(w); }
    if (vb_isinf(w, b)) return 0;
    int fb = w == 32 ? 23 : 52;
    u64 m52 = (1ull << 52) - 1;
    u64 fr = (b & VB_FRC(w)) << (52 - fb);
    int ex = (int)((b >> fb) & (w == 32 ? 0xFF : 0x7FF));
    if (ex == 0) {
        while (!((fr >> 51) & 1)) { fr = (fr << 1) & m52; ex--; }
        fr = (fr << 1) & m52;
    }
    u32 scaled = (ex & 1) == 0 ? 256u + (u32)(fr >> 44) : 128u + (u32)(fr >> 45);
    int rexp = ((w == 32 ? 380 : 3068) - ex) / 2;
    u32 est = vb_rsqrt_est9(scaled);
    u64 f2 = (u64)(est & 0xFF) << 44;
    return w == 32 ? (((u64)(rexp & 0xFF) << 23) | (f2 >> 29)) : (((u64)(rexp & 0x7FF) << 52) | f2);
}

/* round to integral: mode 0 N, 1 P, 2 M, 3 Z, 4 A */
static u64 vb_frint(vb_env *e, int w, u64 b, int mode, int exact) {
    b = vb_fl_in(e, w, b);
    if (vb_isnan(w, b)) return vb_qnan(e, w, b);
    if (vb_isinf(w, b) || vb_iszero(w, b)) return b;
    u64 r;
    int inex;
    if (w == 32) {
        float x = __builtin_bit_cast(float, (u32)b), y;
        __m128 v = _mm_set_ss(x), z = _mm_setzero_ps();
        switch (mode) {
        case 0: y = _mm_cvtss_f32(_mm_round_ss(z, v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)); break;
        case 1: y = _mm_cvtss_f32(_mm_round_ss(z, v, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC)); break;
        case 2: y = _mm_cvtss_f32(_mm_round_ss(z, v, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)); break;
        case 3: y = _mm_cvtss_f32(_mm_round_ss(z, v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)); break;
        default: {
            y = _mm_cvtss_f32(_mm_round_ss(z, v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
            float df = x - y;
            if ((df < 0 ? -df : df) >= 0.5f) y += x < 0 ? -1.0f : 1.0f;
            if (y == 0) y = x < 0 ? -0.0f : 0.0f;
            break;
        }
        }
        inex = y != x;
        r = __builtin_bit_cast(u32, y);
    } else {
        double x = __builtin_bit_cast(double, b), y;
        __m128d v = _mm_set_sd(x), z = _mm_setzero_pd();
        switch (mode) {
        case 0: y = _mm_cvtsd_f64(_mm_round_sd(z, v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)); break;
        case 1: y = _mm_cvtsd_f64(_mm_round_sd(z, v, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC)); break;
        case 2: y = _mm_cvtsd_f64(_mm_round_sd(z, v, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)); break;
        case 3: y = _mm_cvtsd_f64(_mm_round_sd(z, v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)); break;
        default: {
            y = _mm_cvtsd_f64(_mm_round_sd(z, v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
            double df = x - y;
            if ((df < 0 ? -df : df) >= 0.5) y += x < 0 ? -1.0 : 1.0;
            if (y == 0) y = x < 0 ? -0.0 : 0.0;
            break;
        }
        }
        inex = y != x;
        r = __builtin_bit_cast(u64, y);
    }
    if (exact && inex) e->fl |= VB_IXC;
    return r;
}
static const int vb_rm2mode[4] = {0, 1, 2, 3};   /* FPCR.RMode (RN RP RM RZ) -> vb_frint mode */

/* float -> integer lane (rounding mode as vb_frint, fbits fractional bits, saturating). Result is the raw lane. */
static u64 vb_f2i(vb_env *e, int w, u64 b, int lw, int uns, int mode, int fbits) {
    b = vb_fl_in(e, w, b);
    if (vb_isnan(w, b)) { e->fl |= VB_IOC; return 0; }
    double v = w == 32 ? (double)__builtin_bit_cast(float, (u32)b) : __builtin_bit_cast(double, b);
    if (fbits) {
        double sc = __builtin_bit_cast(double, (u64)(1023 + fbits) << 52);
        vb_hbegin(0);
        __asm__ volatile("" : "+x"(v), "+x"(sc));
        v = v * sc;
        __asm__ volatile("" : "+x"(v));
        (void)vb_hend();
    }
    double r;
    {
        __m128d vv = _mm_set_sd(v), z = _mm_setzero_pd();
        switch (mode) {
        case 0: r = _mm_cvtsd_f64(_mm_round_sd(z, vv, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)); break;
        case 1: r = _mm_cvtsd_f64(_mm_round_sd(z, vv, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC)); break;
        case 2: r = _mm_cvtsd_f64(_mm_round_sd(z, vv, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)); break;
        case 3: r = _mm_cvtsd_f64(_mm_round_sd(z, vv, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)); break;
        default: {
            r = _mm_cvtsd_f64(_mm_round_sd(z, vv, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
            double df = v - r;
            if ((df < 0 ? -df : df) >= 0.5) r += v < 0 ? -1.0 : 1.0;
            break;
        }
        }
    }
    u64 mk = lw == 32 ? 0xFFFFFFFFull : ~0ull;
    if (uns) {
        double hi = lw == 32 ? 4294967296.0 : 18446744073709551616.0;
        if (r >= hi) { e->fl |= VB_IOC; return mk; }
        if (r < 0) { e->fl |= VB_IOC; return 0; }
        if (r != v) e->fl |= VB_IXC;
        if (r >= 9223372036854775808.0) return (u64)(s64)(r - 9223372036854775808.0) + 0x8000000000000000ull;
        return (u64)(s64)r & mk;
    }
    double hi = lw == 32 ? 2147483648.0 : 9223372036854775808.0;
    if (r >= hi) { e->fl |= VB_IOC; return (hi == 2147483648.0 ? 0x7FFFFFFFull : 0x7FFFFFFFFFFFFFFFull); }
    if (r < -hi) { e->fl |= VB_IOC; return lw == 32 ? 0x80000000ull : 0x8000000000000000ull; }
    if (r != v) e->fl |= VB_IXC;
    return (u64)(s64)r & mk;
}
/* integer lane -> float (FPCR rounding), fbits fractional bits */
static u64 vb_i2f(vb_env *e, int w, u64 raw, int uns, int fbits) {
    vb_hbegin(e->fpcr);
    u64 r;
    if (w == 32) {
        float f;
        s64 sv = uns ? (s64)(u32)raw : (s64)(s32)raw;
        __asm__ volatile("" : "+r"(sv));
        f = (float)sv;
        __asm__ volatile("" : "+x"(f));
        if (fbits) { float sc = __builtin_bit_cast(float, (u32)((127 - fbits) << 23)); __asm__ volatile("" : "+x"(sc)); f = f * sc; __asm__ volatile("" : "+x"(f)); }
        r = __builtin_bit_cast(u32, f);
    } else {
        double f;
        __asm__ volatile("" : "+r"(raw));
        if (uns) {
            if (raw >> 63) { u64 h = (raw >> 1) | (raw & 1); f = (double)(s64)h; __asm__ volatile("" : "+x"(f)); f = f * 2.0; }
            else f = (double)(s64)raw;
        } else f = (double)(s64)raw;
        __asm__ volatile("" : "+x"(f));
        if (fbits) { double sc = __builtin_bit_cast(double, (u64)(1023 - fbits) << 52); __asm__ volatile("" : "+x"(sc)); f = f * sc; __asm__ volatile("" : "+x"(f)); }
        r = __builtin_bit_cast(u64, f);
    }
    e->fl |= vb_hend();
    return r;
}

/* half <-> single, single <-> double */
static u64 vb_h2s(vb_env *e, u64 hb) {
    u32 h = (u32)hb & 0xFFFF, sg = (h >> 15) << 31, ex = (h >> 10) & 31, fr = h & 0x3FF;
    if (ex == 31) {
        if (!fr) return sg | 0x7F800000u;
        if (!(fr & 0x200)) e->fl |= VB_IOC;
        return VB_DNB(e) ? 0x7FC00000u : (sg | 0x7FC00000u | (fr << 13));
    }
    if (ex == 0) {
        if (!fr) return sg;
        int k = 0;
        while (!(fr & 0x400)) { fr <<= 1; k++; }
        return sg | ((u32)(113 - k) << 23) | ((fr & 0x3FF) << 13);
    }
    return sg | ((ex + 112) << 23) | (fr << 13);
}
static u64 vb_s2h(vb_env *e, u64 b) {
    b = vb_fl_in(e, 32, b);
    u32 s = (u32)b, sg = (s >> 31) << 15, ex = (s >> 23) & 0xFF, fr = s & 0x7FFFFF;
    if (ex == 0xFF) {
        if (!fr) return sg | 0x7C00;
        if (!(fr & 0x400000)) e->fl |= VB_IOC;
        return VB_DNB(e) ? 0x7E00u : (sg | 0x7E00u | (fr >> 13));
    }
    if (ex == 0 && fr == 0) return sg;
    u64 m = ex ? (0x800000u | fr) : fr;
    int e2 = (ex ? (int)ex : 1) - 127;
    int sh = 13 + (e2 < -14 ? -14 - e2 : 0);
    u64 qv, rem, half;
    if (sh >= 40) { qv = 0; rem = m; half = 1ull << 62; }
    else { qv = m >> sh; rem = m & ((1ull << sh) - 1); half = 1ull << (sh - 1); }
    int mode = (e->fpcr >> 22) & 3, up;
    int inexact = rem != 0;
    switch (mode) {
    case 0: up = rem > half || (rem == half && (qv & 1)); break;
    case 1: up = inexact && !sg; break;
    case 2: up = inexact && sg; break;
    default: up = 0; break;
    }
    qv += up;
    u32 res;
    if (e2 >= -14) {
        int be = e2 + 15;
        u32 cand = ((u32)be << 10) + (u32)(qv - 1024);
        if (be >= 31 || cand >= 0x7C00) {
            e->fl |= VB_OFC | VB_IXC;
            int to_inf = mode == 0 || (mode == 1 && !sg) || (mode == 2 && sg);
            return sg | (to_inf ? 0x7C00 : 0x7BFF);
        }
        res = cand;
    } else {
        res = (u32)qv;
        if (inexact) e->fl |= VB_UFC;
    }
    if (inexact) e->fl |= VB_IXC;
    return sg | res;
}
static u64 vb_s2d(vb_env *e, u64 b) {
    b = vb_fl_in(e, 32, b);
    u32 s = (u32)b;
    u64 sg = (u64)(s >> 31) << 63;
    if (vb_isnan(32, s)) {
        if (!(s & 0x400000)) e->fl |= VB_IOC;
        return VB_DNB(e) ? 0x7FF8000000000000ull : (sg | 0x7FF8000000000000ull | ((u64)(s & 0x7FFFFF) << 29));
    }
    return __builtin_bit_cast(u64, (double)__builtin_bit_cast(float, s));
}
static u64 vb_d2s(vb_env *e, u64 b, int odd) {
    b = vb_fl_in(e, 64, b);
    if (vb_isnan(64, b)) {
        if (!(b & VB_QB(64))) e->fl |= VB_IOC;
        return VB_DNB(e) ? 0x7FC00000ull : (((b >> 32) & 0x80000000ull) | 0x7FC00000ull | ((b >> 29) & 0x7FFFFF));
    }
    u32 mode = e->fpcr;
    if (odd) mode = (mode & ~(3u << 22)) | (3u << 22);
    vb_hbegin(mode);
    double x = __builtin_bit_cast(double, b);
    __asm__ volatile("" : "+x"(x));
    float f = (float)x;
    __asm__ volatile("" : "+x"(f));
    u32 fl = vb_hend();
    u64 r = __builtin_bit_cast(u32, f);
    if (odd && (fl & VB_IXC) && !vb_isinf(32, r) && !(r & 1)) r |= 1;
    if (odd && (fl & VB_OFC)) r = (r & 0x80000000ull) | 0x7F7FFFFFull;
    return vb_fin2(e, 32, r, fl, 6, b, 0, 0);
}

/* ---------------------------------------------------------------- vector FP, three-same */
static v128 vb_3fp_slow(Ctx *c, int q, int sz, int a, int u, int opc, v128 vn, v128 vm, v128 vd) {
    vb_env e = {c->fpcr, 0};
    vb_u n = {vn}, m = {vm}, d = {vd}, r = {{0, 0}};
    int w = sz ? 64 : 32, es = sz ? 3 : 2;
    int lanes = sz ? 2 : (q ? 4 : 2);
    int pair = u && (opc == 0x18 || opc == 0x1E || (opc == 0x1A && !a));
    u64 ones = sz ? ~0ull : 0xFFFFFFFFull;
    for (int i = 0; i < lanes; i++) {
        u64 x, y, z = vb_get(&d, es, i), v = 0;
        if (pair) {
            int j = 2 * i, k = 2 * i + 1;
            x = j < lanes ? vb_get(&n, es, j) : vb_get(&m, es, j - lanes);
            y = k < lanes ? vb_get(&n, es, k) : vb_get(&m, es, k - lanes);
        } else { x = vb_get(&n, es, i); y = vb_get(&m, es, i); }
        if (!u) {
            switch (opc) {
            case 0x18: v = vb_fmaxmin(&e, w, a, 1, x, y); break;
            case 0x19: v = vb_ffma(&e, w, z, a ? x ^ VB_SGN(w) : x, y); break;
            case 0x1A: v = vb_fbin(&e, w, a ? 1 : 0, x, y); break;
            case 0x1B: v = vb_fmulx(&e, w, x, y); break;
            case 0x1C: v = vb_fcmp(&e, w, 0, x, y) ? ones : 0; break;
            case 0x1E: v = vb_fmaxmin(&e, w, a, 0, x, y); break;
            default: v = a ? vb_frsqrts(&e, w, x, y) : vb_frecps(&e, w, x, y); break;
            }
        } else {
            switch (opc) {
            case 0x18: v = vb_fmaxmin(&e, w, a, 1, x, y); break;
            case 0x1A: v = a ? (vb_fbin(&e, w, 1, x, y) & ~VB_SGN(w)) : vb_fbin(&e, w, 0, x, y); break;
            case 0x1B: v = vb_fbin(&e, w, 2, x, y); break;
            case 0x1C: v = vb_fcmp(&e, w, a ? 2 : 1, x, y) ? ones : 0; break;
            case 0x1D: v = vb_fcmp(&e, w, a ? 2 : 1, x & ~VB_SGN(w), y & ~VB_SGN(w)) ? ones : 0; break;
            case 0x1E: v = vb_fmaxmin(&e, w, a, 0, x, y); break;
            default: v = vb_fbin(&e, w, 3, x, y); break;
            }
        }
        vb_put(&r, es, i, v);
    }
    c->fpsr |= e.fl;
    return r.v;
}

/* fast path: FPCR default modes, no NaN inputs, single precision; falls back to the scalar engine otherwise */
VB_INL int vb_3fp_kind(int u, int a, int opc) {
    if (!u) {
        switch (opc) {
        case 0x18: return a ? 9 : 8;
        case 0x19: return a ? 7 : 6;
        case 0x1A: return a ? 2 : 1;
        case 0x1C: return a ? 0 : 10;
        case 0x1E: return a ? 9 : 8;
        default: return 0;
        }
    }
    switch (opc) {
    case 0x1A: return a ? 5 : 15;
    case 0x1B: return a ? 0 : 3;
    case 0x1C: return a ? 12 : 11;
    case 0x1D: return a ? 14 : 13;
    case 0x1F: return a ? 0 : 4;
    default: return 0;
    }
}
VB_INL v128 vb_3fp(Ctx *c, int q, int sz, int a, int u, int opc, v128 n, v128 m, v128 d) {
    int kind = vb_3fp_kind(u, a, opc);
    if (!sz && kind && !(c->fpcr & 0x03C00000u)) {
        __m128 x = (__m128)n, y = (__m128)m, z = (__m128)d, one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
        if (kind == 15) {
            __m128 t = _mm_unpacklo_ps(x, y);
            if (q) { __m128 xx = x, yy = y; x = _mm_shuffle_ps(xx, yy, _MM_SHUFFLE(2, 0, 2, 0)); y = _mm_shuffle_ps(xx, yy, _MM_SHUFFLE(3, 1, 3, 1)); }
            else { x = _mm_movelh_ps(t, zero); y = _mm_movelh_ps(_mm_movehl_ps(t, t), zero); }
        } else if (!q) { x = _mm_movelh_ps(x, one); y = _mm_movelh_ps(y, one); z = _mm_movelh_ps(z, zero); }
        const __m128i absm = _mm_set1_epi32(0x7FFFFFFF), infb = _mm_set1_epi32(0x7F800000);
        int bad = _mm_movemask_epi8(_mm_cmpgt_epi32(_mm_and_si128((__m128i)x, absm), infb)) |
                  _mm_movemask_epi8(_mm_cmpgt_epi32(_mm_and_si128((__m128i)y, absm), infb));
        if (kind == 6 || kind == 7) bad |= _mm_movemask_epi8(_mm_cmpgt_epi32(_mm_and_si128((__m128i)z, absm), infb));
        if (!bad) {
            __asm__ volatile("ldmxcsr %0" ::"m"(vb_mx_default) : "memory");
            __asm__ volatile("" : "+x"(x), "+x"(y), "+x"(z));
            __m128 r;
            __m128 sgn = _mm_set1_ps(-0.0f);
            switch (kind) {
            case 1: case 15: r = _mm_add_ps(x, y); break;
            case 2: r = _mm_sub_ps(x, y); break;
            case 3: r = _mm_mul_ps(x, y); break;
            case 4: r = _mm_div_ps(x, y); break;
            case 5: r = _mm_andnot_ps(sgn, _mm_sub_ps(x, y)); break;
            case 6: r = _mm_fmadd_ps(x, y, z); break;
            case 7: r = _mm_fnmadd_ps(x, y, z); break;
            case 8: {
                __m128 g = _mm_cmpgt_ps(x, y), eq = _mm_cmpeq_ps(x, y);
                r = _mm_blendv_ps(_mm_blendv_ps(y, x, g), _mm_and_ps(x, y), eq);
                break;
            }
            case 9: {
                __m128 g = _mm_cmplt_ps(x, y), eq = _mm_cmpeq_ps(x, y);
                r = _mm_blendv_ps(_mm_blendv_ps(y, x, g), _mm_or_ps(x, y), eq);
                break;
            }
            case 10: r = _mm_cmpeq_ps(x, y); break;
            case 11: r = _mm_cmpge_ps(x, y); break;
            case 12: r = _mm_cmpgt_ps(x, y); break;
            case 13: r = _mm_cmpge_ps(_mm_andnot_ps(sgn, x), _mm_andnot_ps(sgn, y)); break;
            default: r = _mm_cmpgt_ps(_mm_andnot_ps(sgn, x), _mm_andnot_ps(sgn, y)); break;
            }
            __asm__ volatile("" : "+x"(r));
            u32 mx;
            __asm__ volatile("stmxcsr %0" : "=m"(mx)::"memory");
            if (kind < 10) {
                __m128i ab = _mm_and_si128((__m128i)r, absm);
                if ((mx & 0x20) && _mm_movemask_epi8(_mm_cmpeq_epi32(ab, _mm_set1_epi32(0x00800000)))) return vb_3fp_slow(c, q, sz, a, u, opc, n, m, d);
                r = _mm_blendv_ps(r, _mm_castsi128_ps(_mm_set1_epi32(0x7FC00000)), (__m128)_mm_cmpgt_epi32(ab, infb));
            }
            c->fpsr |= (mx & 1) | ((mx >> 1) & 0x1E);
            return vb_q(q, (v128)r);
        }
    }
    return vb_3fp_slow(c, q, sz, a, u, opc, n, m, d);
}

/* ---------------------------------------------------------------- vector FP, two-register misc */
static v128 vb_2fp_slow(Ctx *c, int q, int sz, int a, int u, int opc, v128 vn, v128 vd) {
    vb_env e = {c->fpcr, 0};
    vb_u n = {vn}, d = {vd}, r = {{0, 0}};
    int w = sz ? 64 : 32, es = sz ? 3 : 2;
    int lanes = sz ? 2 : (q ? 4 : 2);
    u64 ones = sz ? ~0ull : 0xFFFFFFFFull;
    if (opc == 0x16) {                           /* FCVTN / FCVTXN */
        if (q) r.d[0] = d.d[0];
        if (!sz) { for (int i = 0; i < 4; i++) r.h[(q ? 4 : 0) + i] = (u16)vb_s2h(&e, n.s[i]); }
        else for (int i = 0; i < 2; i++) r.s[(q ? 2 : 0) + i] = (u32)vb_d2s(&e, n.d[i], u);
        c->fpsr |= e.fl;
        return r.v;
    }
    if (opc == 0x17) {                           /* FCVTL */
        if (!sz) { for (int i = 0; i < 4; i++) r.s[i] = (u32)vb_h2s(&e, n.h[(q ? 4 : 0) + i]); }
        else for (int i = 0; i < 2; i++) r.d[i] = vb_s2d(&e, n.s[(q ? 2 : 0) + i]);
        c->fpsr |= e.fl;
        return r.v;
    }
    for (int i = 0; i < lanes; i++) {
        u64 x = vb_get(&n, es, i), v = 0;
        int rm = (e.fpcr >> 22) & 3;
        switch (opc) {
        case 0x0C: v = vb_fcmp(&e, w, u ? 1 : 2, x, 0) ? ones : 0; break;
        case 0x0D: v = u ? (vb_fcmp(&e, w, 1, 0, x) ? ones : 0) : (vb_fcmp(&e, w, 0, x, 0) ? ones : 0); break;
        case 0x0E: v = vb_fcmp(&e, w, 2, 0, x) ? ones : 0; break;
        case 0x0F: v = u ? (x ^ VB_SGN(w)) : (x & ~VB_SGN(w)); break;
        case 0x18: v = vb_frint(&e, w, x, u ? 4 : (a ? 1 : 0), 0); break;
        case 0x19: v = u ? vb_frint(&e, w, x, vb_rm2mode[rm], !a) : vb_frint(&e, w, x, a ? 3 : 2, 0); break;
        case 0x1A: v = vb_f2i(&e, w, x, w, u, a ? 1 : 0, 0); break;
        case 0x1B: v = vb_f2i(&e, w, x, w, u, a ? 3 : 2, 0); break;
        case 0x1C:
            if (!a) v = vb_f2i(&e, w, x, w, u, 4, 0);
            break;
        case 0x1D:
            if (!a) v = vb_i2f(&e, w, x, u, 0);
            else v = u ? vb_frsqrte(&e, w, x) : vb_frecpe(&e, w, x);
            break;
        default: v = vb_fsqrt(&e, w, x); break;
        }
        if (opc == 0x1C && a) {                 /* URECPE / URSQRTE: integer estimates */
            u32 xv = (u32)x;
            if (!u) v = (xv >> 31) ? ((u64)(vb_recip_est9(xv >> 23) & 0x1FF) << 23) : 0xFFFFFFFFull;
            else v = (xv >> 30) ? ((u64)(vb_rsqrt_est9(xv >> 23) & 0x1FF) << 23) : 0xFFFFFFFFull;
        }
        vb_put(&r, es, i, v);
    }
    c->fpsr |= e.fl;
    return r.v;
}
VB_INL v128 vb_2fp(Ctx *c, int q, int sz, int a, int u, int opc, v128 n, v128 d) {
    if (opc == 0x0F) {
        v128 k = sz ? (v128){(long long)0x8000000000000000ull, (long long)0x8000000000000000ull} : (v128){(long long)0x8000000080000000ull, (long long)0x8000000080000000ull};
        return vb_q(q, u ? (n ^ k) : (n & ~k));
    }
    return vb_2fp_slow(c, q, sz, a, u, opc, n, d);
}

/* ---------------------------------------------------------------- fixed-point conversion, across lanes FP, indexed element */
static v128 vb_fixcvt(Ctx *c, int q, int sz, int to_int, int u, int fbits, v128 vn) {
    vb_env e = {c->fpcr, 0};
    vb_u n = {vn}, r = {{0, 0}};
    int w = sz ? 64 : 32, es = sz ? 3 : 2, lanes = sz ? 2 : (q ? 4 : 2);
    for (int i = 0; i < lanes; i++) {
        u64 x = vb_get(&n, es, i);
        vb_put(&r, es, i, to_int ? vb_f2i(&e, w, x, w, u, 3, fbits) : vb_i2f(&e, w, x, u, fbits));
    }
    c->fpsr |= e.fl;
    return r.v;
}
static v128 vb_across_fp(Ctx *c, int q, int size, int u, int opc, v128 vn) {
    vb_env e = {c->fpcr, 0};
    vb_u n = {vn}, r = {{0, 0}};
    int ismin = size >> 1, num = opc == 0x0C;
    u64 x0 = n.s[0], x1 = n.s[1], x2 = n.s[2], x3 = n.s[3];
    u64 a01 = vb_fmaxmin(&e, 32, ismin, num, x0, x1), a23 = vb_fmaxmin(&e, 32, ismin, num, x2, x3);
    r.s[0] = (u32)vb_fmaxmin(&e, 32, ismin, num, a01, a23);
    c->fpsr |= e.fl;
    return r.v;
}
VB_INL v128 vb_across_all(Ctx *c, int q, int size, int u, int opc, v128 vn) {
    if (opc == 0x0C || opc == 0x0F) return vb_across_fp(c, q, size, u, opc, vn);
    return vb_across(c, q, size, u, opc, vn);
}
VB_INL v128 vb_elemf(Ctx *c, int q, int sz, int u, int opc, int idx, v128 n, v128 m, v128 d) {
    v128 mm = vb_dupe(1, sz ? 3 : 2, idx, m);
    switch (opc) {
    case 1: return vb_3fp(c, q, sz, 0, 0, 0x19, n, mm, d);
    case 5: return vb_3fp(c, q, sz, 1, 0, 0x19, n, mm, d);
    default: return u ? vb_3fp(c, q, sz, 0, 0, 0x1B, n, mm, d) : vb_3fp(c, q, sz, 0, 1, 0x1B, n, mm, d);
    }
}
VB_INL v128 vb_elem(Ctx *c, int q, int size, int u, int opc, int idx, v128 n, v128 m, v128 d) {
    v128 mm = vb_dupe(1, size, idx, m);
    switch (opc) {
    case 8: return vb_3int(c, q, size, 0, 0x13, n, mm, d);
    case 0: return vb_3int(c, q, size, 0, 0x12, n, mm, d);
    case 4: return vb_3int(c, q, size, 1, 0x12, n, mm, d);
    case 12: return vb_3int(c, q, size, 0, 0x16, n, mm, d);
    case 13: return vb_3int(c, q, size, 1, 0x16, n, mm, d);
    case 2: return vb_3diff(c, q, size, u, 8, n, mm, d);
    case 6: return vb_3diff(c, q, size, u, 10, n, mm, d);
    case 10: return vb_3diff(c, q, size, u, 12, n, mm, d);
    case 3: return vb_3diff(c, q, size, 0, 9, n, mm, d);
    case 7: return vb_3diff(c, q, size, 0, 11, n, mm, d);
    default: return vb_3diff(c, q, size, 0, 13, n, mm, d);
    }
}
