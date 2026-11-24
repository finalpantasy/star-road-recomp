/* IR operation semantics helpers (task-5). Our own code. One definition, three users:
 * the C emitter (lifted code), the IR interpreter, and the oracle tests (docs/RECOMPILER.md 3.2).
 * The per-op macros OP_<name> are generated from tools/lifter/ops.py into ops_gen.h and use these helpers
 * plus the register-access macros defined by mode_lift.h (locals) or mode_interp.h (Ctx fields). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "rt.h"

typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t   s8;  typedef int16_t  s16; typedef int32_t  s32; typedef int64_t  s64;
typedef unsigned __int128 u128; typedef __int128 s128;
typedef long long v128u __attribute__((vector_size(16), aligned(1)));

uint64_t rt_cntvct(void);

/* ---- guest memory: identity mapped, relaxed atomics (doc 3.10) ---- */
#define MLD(T, a) __atomic_load_n((T *)(uintptr_t)(a), __ATOMIC_RELAXED)
#define MST(T, a, v) __atomic_store_n((T *)(uintptr_t)(a), (T)(v), __ATOMIC_RELAXED)

static inline u64 ir_m(int sf, u64 v) { return sf ? v : (u32)v; }

static inline u64 ir_ld(int sz, int sgn, int d64, u64 a) {
    u64 v;
    switch (sz) {
    case 1: v = sgn ? (u64)(s64)MLD(s8, a) : MLD(u8, a); break;
    case 2: v = sgn ? (u64)(s64)MLD(s16, a) : MLD(u16, a); break;
    case 4: v = sgn ? (u64)(s64)MLD(s32, a) : MLD(u32, a); break;
    default: v = MLD(u64, a); break;
    }
    return d64 ? v : (u32)v;
}
static inline void ir_st(int sz, u64 a, u64 v) {
    switch (sz) {
    case 1: MST(u8, a, v); break;
    case 2: MST(u16, a, v); break;
    case 4: MST(u32, a, v); break;
    default: MST(u64, a, v); break;
    }
}
static inline v128 ir_ldv(int sz, u64 a) {
    v128 r = {0, 0};
    switch (sz) {
    case 1: r[0] = MLD(u8, a); break;
    case 2: r[0] = MLD(u16, a); break;
    case 4: r[0] = MLD(u32, a); break;
    case 8: r[0] = (long long)MLD(u64, a); break;
    default: r = (v128)*(v128u *)(uintptr_t)a; break;
    }
    return r;
}
static inline void ir_stv(int sz, u64 a, v128 v) {
    switch (sz) {
    case 1: MST(u8, a, v[0]); break;
    case 2: MST(u16, a, v[0]); break;
    case 4: MST(u32, a, v[0]); break;
    case 8: MST(u64, a, v[0]); break;
    default: *(v128u *)(uintptr_t)a = (v128u)v; break;
    }
}

/* ---- integer arithmetic ---- */
static inline u64 ir_addc(int sf, u64 a, u64 b, unsigned cin, u8 *fl) {
    u64 r; unsigned cy; unsigned ov;
    if (sf) {
        u64 t; cy = __builtin_add_overflow(a, b, &t); cy |= __builtin_add_overflow(t, (u64)cin, &r);
        ov = (unsigned)(((a ^ r) & (b ^ r)) >> 63) & 1;
        fl[0] = (u8)(r >> 63); fl[1] = r == 0;
    } else {
        u32 a32 = (u32)a, b32 = (u32)b, t32, r32;
        cy = __builtin_add_overflow(a32, b32, &t32); cy |= __builtin_add_overflow(t32, (u32)cin, &r32);
        ov = ((a32 ^ r32) & (b32 ^ r32)) >> 31; r = r32;
        fl[0] = (u8)(r32 >> 31); fl[1] = r32 == 0;
    }
    fl[2] = (u8)cy; fl[3] = (u8)ov;
    return r;
}
static inline u64 ir_addsub(int sf, int sub, u64 a, u64 b) { return ir_m(sf, sub ? a - b : a + b); }
static inline u64 ir_addsubs(int sf, int sub, u64 a, u64 b, u8 *fl) { return ir_addc(sf, a, sub ? ~b : b, sub, fl); }
static inline u64 ir_adc(int sf, int sub, u64 a, u64 b, u8 cin) { return ir_m(sf, a + (sub ? ~b : b) + cin); }
static inline u64 ir_adcs(int sf, int sub, u64 a, u64 b, u8 cin, u8 *fl) { return ir_addc(sf, a, sub ? ~b : b, cin, fl); }

static inline int ir_cond(const u8 *fl, int cond) {
    int n = fl[0], z = fl[1], c = fl[2], v = fl[3], r;
    switch (cond >> 1) {
    case 0: r = z; break; case 1: r = c; break; case 2: r = n; break; case 3: r = v; break;
    case 4: r = c && !z; break; case 5: r = n == v; break; case 6: r = !z && n == v; break;
    default: return 1;
    }
    return (cond & 1) ? !r : r;
}
static inline u64 ir_shift(int sf, int type, u64 v, unsigned amt) {
    int bits = sf ? 64 : 32;
    amt &= bits - 1;
    if (!sf) v = (u32)v;
    switch (type) {
    case 0: return ir_m(sf, v << amt);
    case 1: return v >> amt;
    case 2: return sf ? (u64)((s64)v >> amt) : (u64)(u32)((s32)(u32)v >> amt);
    default: return amt ? ir_m(sf, (v >> amt) | (v << (bits - amt))) : v;
    }
}
static inline u64 ir_ext(u64 v, int opt, int amt) {
    switch (opt) {
    case 0: v = (u8)v; break; case 1: v = (u16)v; break; case 2: v = (u32)v; break; case 3: break;
    case 4: v = (u64)(s64)(s8)v; break; case 5: v = (u64)(s64)(s16)v; break; case 6: v = (u64)(s64)(s32)v; break;
    default: break;
    }
    return v << amt;
}
static inline u64 ir_log(int sf, int opc, u64 a, u64 b) {
    return ir_m(sf, opc == 0 ? (a & b) : opc == 1 ? (a | b) : (a ^ b));
}
static inline u64 ir_ands(int sf, u64 a, u64 b, u8 *fl) {
    u64 r = ir_m(sf, a & b);
    fl[0] = (u8)(r >> (sf ? 63 : 31)); fl[1] = r == 0; fl[2] = 0; fl[3] = 0;
    return r;
}
static inline u64 ir_bfm(int sf, int opc, u64 dst, u64 src, int immr, int imms, u64 wmask, u64 tmask) {
    int bits = sf ? 64 : 32;
    if (!sf) src = (u32)src;
    u64 rot = immr ? ((src >> immr) | (src << (bits - immr))) : src;
    if (!sf) rot = (u32)rot;
    u64 d = opc == 1 ? dst : 0;
    u64 bot = (d & ~wmask) | (rot & wmask);
    u64 top;
    if (opc == 0) top = ((src >> imms) & 1) ? ~0ull : 0; else if (opc == 1) top = d; else top = 0;
    return ir_m(sf, (top & ~tmask) | (bot & tmask));
}
static inline u64 ir_extr(int sf, u64 hi, u64 lo, int lsb) {
    if (sf) return lsb ? (lo >> lsb) | (hi << (64 - lsb)) : lo;
    u64 cat = ((u64)(u32)hi << 32) | (u32)lo; return (u32)(cat >> lsb);
}
static inline u64 ir_dp2(int sf, int kind, u64 a, u64 b) {
    switch (kind) {
    case 2: if (!sf) { return (u32)b ? (u32)a / (u32)b : 0; } return b ? a / b : 0;
    case 3:
        if (!sf) { s32 x = (s32)a, y = (s32)b; return y == 0 ? 0 : (x == INT32_MIN && y == -1) ? (u32)x : (u32)(x / y); }
        { s64 x = (s64)a, y = (s64)b; return y == 0 ? 0 : (x == INT64_MIN && y == -1) ? (u64)x : (u64)(x / y); }
    case 8: return ir_shift(sf, 0, a, (unsigned)b);
    case 9: return ir_shift(sf, 1, a, (unsigned)b);
    case 10: return ir_shift(sf, 2, a, (unsigned)b);
    default: return ir_shift(sf, 3, a, (unsigned)b);
    }
}
static inline u64 ir_rbit(int sf, u64 a) {
    u64 r = 0; int n = sf ? 64 : 32;
    for (int i = 0; i < n; i++) r |= ((a >> i) & 1) << (n - 1 - i);
    return r;
}
static inline u64 ir_dp1(int sf, int kind, u64 a) {
    int n = sf ? 64 : 32;
    switch (kind) {
    case 0: return ir_rbit(sf, a);
    case 1: { u64 r = 0; for (int i = 0; i < n / 16; i++) r |= (u64)__builtin_bswap16((u16)(a >> (16 * i))) << (16 * i); return r; }
    case 2: return sf ? ((u64)__builtin_bswap32((u32)(a >> 32)) << 32 | __builtin_bswap32((u32)a)) : __builtin_bswap32((u32)a);
    case 3: return __builtin_bswap64(a);
    case 4: { u64 v = sf ? a : (u32)a; return v == 0 ? (u64)n : (u64)(__builtin_clzll(v) - (sf ? 0 : 32)); }
    default: { /* cls */
        u64 v = sf ? a : (u32)a; int sb = (int)((v >> (n - 1)) & 1);
        u64 x = sb ? ~v : v; if (!sf) x &= 0xFFFFFFFFull;
        int lz = x == 0 ? n : __builtin_clzll(x) - (sf ? 0 : 32);
        return (u64)(lz - 1);
    }
    }
}
static inline u64 ir_dp3(int sf, int kind, u64 a, u64 b, u64 acc) {
    switch (kind) {
    case 0: return ir_m(sf, acc + a * b);
    case 1: return ir_m(sf, acc - a * b);
    case 2: return acc + (u64)((s64)(s32)a * (s64)(s32)b);
    case 3: return acc - (u64)((s64)(s32)a * (s64)(s32)b);
    case 4: return (u64)(((s128)(s64)a * (s128)(s64)b) >> 64);
    case 5: return acc + (u64)(u32)a * (u64)(u32)b;
    case 6: return acc - (u64)(u32)a * (u64)(u32)b;
    default: return (u64)(((u128)a * (u128)b) >> 64);
    }
}
static inline u64 ir_csel(int sf, int kind, int cond_true, u64 a, u64 b) {
    if (cond_true) return ir_m(sf, a);
    switch (kind) { case 0: return ir_m(sf, b); case 1: return ir_m(sf, b + 1); case 2: return ir_m(sf, ~b); default: return ir_m(sf, 0 - b); }
}
static inline void ir_ccmp(int sf, int sub, int cond_true, u64 a, u64 b, int nzcv, u8 *fl) {
    if (cond_true) { u8 t[4]; (void)ir_addc(sf, a, sub ? ~b : b, sub, t); memcpy(fl, t, 4); }
    else { fl[0] = (nzcv >> 3) & 1; fl[1] = (nzcv >> 2) & 1; fl[2] = (nzcv >> 1) & 1; fl[3] = nzcv & 1; }
}

/* ---- barriers ---- */
static inline void ir_dmb(int crm) {
    if (crm == 0xF || crm == 0xB || crm == 0x3 || crm == 0xE || crm == 0x7) __atomic_thread_fence(__ATOMIC_SEQ_CST);
    else __atomic_signal_fence(__ATOMIC_SEQ_CST);
}

/* ---- system registers (o0:op1:CRn:CRm:op2 packed as b[20:5] of the instruction) ---- */
#define SYS_NZCV 0xDA10
#define SYS_FPCR 0xDA20
#define SYS_FPSR 0xDA21
#define SYS_TPIDR_EL0 0xDE82
#define SYS_TPIDRRO_EL0 0xDE83
#define SYS_CNTFRQ 0xDF00
#define SYS_CNTVCT 0xDF02
#define SYS_DCZID 0xD807
#define SYS_CTR 0xD801

/* ---- acquire/release, CAS, LSE atomics ---- */
static inline u64 ir_ldar(int sz, u64 a) {
    switch (sz) {
    case 1: return __atomic_load_n((u8 *)(uintptr_t)a, __ATOMIC_ACQUIRE);
    case 2: return __atomic_load_n((u16 *)(uintptr_t)a, __ATOMIC_ACQUIRE);
    case 4: return __atomic_load_n((u32 *)(uintptr_t)a, __ATOMIC_ACQUIRE);
    default: return __atomic_load_n((u64 *)(uintptr_t)a, __ATOMIC_ACQUIRE);
    }
}
static inline void ir_stlr(int sz, u64 a, u64 v) {
    switch (sz) {
    case 1: __atomic_store_n((u8 *)(uintptr_t)a, (u8)v, __ATOMIC_SEQ_CST); break;
    case 2: __atomic_store_n((u16 *)(uintptr_t)a, (u16)v, __ATOMIC_SEQ_CST); break;
    case 4: __atomic_store_n((u32 *)(uintptr_t)a, (u32)v, __ATOMIC_SEQ_CST); break;
    default: __atomic_store_n((u64 *)(uintptr_t)a, (u64)v, __ATOMIC_SEQ_CST); break;
    }
}
/* compare-and-swap returning the old value */
static inline u64 ir_cas_old(int sz, u64 a, u64 expect, u64 desired) {
    switch (sz) {
    case 1: { u8 e = (u8)expect; __atomic_compare_exchange_n((u8 *)(uintptr_t)a, &e, (u8)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    case 2: { u16 e = (u16)expect; __atomic_compare_exchange_n((u16 *)(uintptr_t)a, &e, (u16)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    case 4: { u32 e = (u32)expect; __atomic_compare_exchange_n((u32 *)(uintptr_t)a, &e, (u32)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    default: { u64 e = expect; __atomic_compare_exchange_n((u64 *)(uintptr_t)a, &e, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    }
}
static inline int ir_cas(int sz, u64 a, u64 expect, u64 desired) {
    u64 m = sz == 8 ? ~0ull : ((1ull << (8 * sz)) - 1);
    return ir_cas_old(sz, a, expect, desired) == (expect & m);
}
static inline void ir_casp_old(int sz, u64 a, u64 e1, u64 e2, u64 d1, u64 d2, u64 *o1, u64 *o2) {
    if (sz == 8) {
        u128 e = ((u128)e2 << 64) | e1, d = ((u128)d2 << 64) | d1;
        __atomic_compare_exchange_n((u128 *)(uintptr_t)a, &e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *o1 = (u64)e; *o2 = (u64)(e >> 64);
    } else {
        u64 e = ((u64)(u32)e2 << 32) | (u32)e1, d = ((u64)(u32)d2 << 32) | (u32)d1;
        __atomic_compare_exchange_n((u64 *)(uintptr_t)a, &e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *o1 = (u32)e; *o2 = (u32)(e >> 32);
    }
}
static inline int ir_casp(int sz, u64 a, u64 e1, u64 e2, u64 d1, u64 d2) {
    u64 o1, o2; ir_casp_old(sz, a, e1, e2, d1, d2, &o1, &o2);
    u64 m = sz == 8 ? ~0ull : 0xFFFFFFFFull;
    return o1 == (e1 & m) && o2 == (e2 & m);
}
/* LSE read-modify-write: opc 0 add, 1 clr, 2 eor, 3 set, 4 smax, 5 smin, 6 umax, 7 umin, 8 swp; returns old value */
static inline u64 ir_atomic(int sz, int opc, u64 a, u64 v) {
    u64 m = sz == 8 ? ~0ull : ((1ull << (8 * sz)) - 1);
    u64 old = ir_ldar(sz, a);
    for (;;) {
        u64 nv;
        int sh = 64 - 8 * sz;
        switch (opc) {
        case 0: nv = old + v; break;
        case 1: nv = old & ~v; break;
        case 2: nv = old ^ v; break;
        case 3: nv = old | v; break;
        case 4: nv = ((s64)(old << sh) >> sh) > ((s64)(v << sh) >> sh) ? old : v; break;
        case 5: nv = ((s64)(old << sh) >> sh) < ((s64)(v << sh) >> sh) ? old : v; break;
        case 6: nv = (old & m) > (v & m) ? old : v; break;
        case 7: nv = (old & m) < (v & m) ? old : v; break;
        default: nv = v; break;
        }
        u64 got = ir_cas_old(sz, a, old, nv);
        if (got == (old & m)) return old & m;
        old = got;
    }
}

/* Extension helpers (scalar FP, Advanced SIMD); each file is optional so a worker can build with its own only. */
#if __has_include("ir_simd_a.h") && !defined(NO_SIMD_A)
#include "ir_simd_a.h"
#endif
#if __has_include("ir_simd_b.h") && !defined(NO_SIMD_B)
#include "ir_simd_b.h"
#endif
