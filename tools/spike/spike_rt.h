/* Runtime header for the task-4 lifter spike. Our own code. */
#pragma once
#include <stdint.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t i8; typedef int16_t i16; typedef int32_t i32; typedef int64_t i64;
typedef __int128 i128; typedef unsigned __int128 u128;

typedef struct { u64 l, h; } V128;
typedef struct Ctx {
    u64 x[31]; u64 sp; u64 pc;
    u8 fn, fz, fc, fv;
    V128 v[32];
    u64 tpidr, tpidrro, fpcr, fpsr;
    u64 excl_addr, excl_val;
    u64 exc; /* pending guest C++ exception object, 0 if none */
} Ctx;

/* ---- guest memory: identity (guest address == host address) or base plus 39-bit mask ---- */
#if MEM_MASKED
extern u8 *g_membase;
#define GP(a) (g_membase + ((u64)(a) & 0x7FFFFFFFFFULL))
#else
#define GP(a) ((u8 *)(uintptr_t)(u64)(a))
#endif

#if ACCESS_RELAXED
#define LDX_(T, a) __atomic_load_n((T *)GP(a), __ATOMIC_RELAXED)
#define STX_(T, a, v) __atomic_store_n((T *)GP(a), (T)(v), __ATOMIC_RELAXED)
#else
#define LDX_(T, a) ({ T v_; __builtin_memcpy(&v_, GP(a), sizeof(T)); v_; })
#define STX_(T, a, v) do { T v_ = (T)(v); __builtin_memcpy(GP(a), &v_, sizeof(T)); } while (0)
#endif
#define LD8(a) LDX_(u8, a)
#define LD16(a) LDX_(u16, a)
#define LD32(a) LDX_(u32, a)
#define LD64(a) LDX_(u64, a)
#define ST8(a, v) STX_(u8, a, v)
#define ST16(a, v) STX_(u16, a, v)
#define ST32(a, v) STX_(u32, a, v)
#define ST64(a, v) STX_(u64, a, v)
/* LDAR is acquire; STLR must be seq_cst on x86 (store-load order between STLR and a later LDAR). */
#define LDA8(a) __atomic_load_n((u8 *)GP(a), __ATOMIC_ACQUIRE)
#define LDA16(a) __atomic_load_n((u16 *)GP(a), __ATOMIC_ACQUIRE)
#define LDA32(a) __atomic_load_n((u32 *)GP(a), __ATOMIC_ACQUIRE)
#define LDA64(a) __atomic_load_n((u64 *)GP(a), __ATOMIC_ACQUIRE)
#define STL8(a, v) __atomic_store_n((u8 *)GP(a), (u8)(v), __ATOMIC_SEQ_CST)
#define STL16(a, v) __atomic_store_n((u16 *)GP(a), (u16)(v), __ATOMIC_SEQ_CST)
#define STL32(a, v) __atomic_store_n((u32 *)GP(a), (u32)(v), __ATOMIC_SEQ_CST)
#define STL64(a, v) __atomic_store_n((u64 *)GP(a), (u64)(v), __ATOMIC_SEQ_CST)

#define STXR_(N, T) static inline u32 rt_stxr##N(Ctx *c, u64 a, T v) { \
    if (c->excl_addr != a) return 1; T e = (T)c->excl_val; c->excl_addr = ~0ULL; \
    return __atomic_compare_exchange_n((T *)GP(a), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1; }
STXR_(8, u8) STXR_(16, u16) STXR_(32, u32) STXR_(64, u64)

/* ---- runtime entry points ---- */
void rt_icall(Ctx *c, u64 target);
void rt_dispatch(Ctx *c);
void rt_svc(Ctx *c, u32 imm);
void rt_brk(Ctx *c, u32 imm);
void rt_safepoint(Ctx *c);
u64 rt_cntvct(void);
extern volatile int g_rt_poll;

/* ---- integer helpers ---- */
static inline u32 ror32(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline u64 ror64(u64 v, u64 s) { s &= 63; return s ? (v >> s) | (v << (64 - s)) : v; }
static inline u32 udiv32(u32 a, u32 b) { return b ? a / b : 0; }
static inline u64 udiv64(u64 a, u64 b) { return b ? a / b : 0; }
static inline u32 sdiv32(u32 a, u32 b) { if (!b) return 0; if ((i32)a == INT32_MIN && (i32)b == -1) return a; return (u32)((i32)a / (i32)b); }
static inline u64 sdiv64(u64 a, u64 b) { if (!b) return 0; if ((i64)a == INT64_MIN && (i64)b == -1) return a; return (u64)((i64)a / (i64)b); }
static inline u32 rev16_32(u32 v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }
static inline u64 rev16_64(u64 v) { return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull); }
static inline u64 rev32_64(u64 v) { return ((u64)__builtin_bswap32((u32)(v >> 32)) << 32) | __builtin_bswap32((u32)v); }
static inline u32 clz32(u32 v) { return v ? __builtin_clz(v) : 32; }
static inline u64 clz64(u64 v) { return v ? __builtin_clzll(v) : 64; }
static inline u32 cls32(u32 v) { return clz32(v ^ (u32)((i32)v >> 31)) - 1 + (v == 0 || v == 0xffffffffu ? 0 : 0); }
static inline u64 cls64(u64 v) { return clz64(v ^ (u64)((i64)v >> 63)) - 1; }

/* ---- FP helpers (spike precision: NaN payloads and FPCR modes are not modelled yet) ---- */
static inline float u2f(u64 v) { u32 b = (u32)v; float f; __builtin_memcpy(&f, &b, 4); return f; }
static inline double u2d(u64 v) { double d; __builtin_memcpy(&d, &v, 8); return d; }
static inline u32 f2u(float f) { u32 b; __builtin_memcpy(&b, &f, 4); return b; }
static inline u64 d2u(double d) { u64 b; __builtin_memcpy(&b, &d, 8); return b; }
#define FCMP_(S, T) static inline u32 fcmp_##S(T a, T b) { if (a != a || b != b) return 3; if (a == b) return 6; return a < b ? 8 : 2; }
FCMP_(f, float) FCMP_(d, double)
#define FMM_(S, T) \
  static inline T fmax_##S(T a, T b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return __builtin_signbit(a) ? b : a; return a > b ? a : b; } \
  static inline T fmin_##S(T a, T b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return __builtin_signbit(a) ? a : b; return a < b ? a : b; } \
  static inline T fmaxnm_##S(T a, T b) { if (a != a) return b; if (b != b) return a; return fmax_##S(a, b); } \
  static inline T fminnm_##S(T a, T b) { if (a != a) return b; if (b != b) return a; return fmin_##S(a, b); }
FMM_(f, float) FMM_(d, double)
static inline double rnd_(double x, char r) {
    switch (r) { case 'n': return __builtin_roundeven(x); case 'p': return __builtin_ceil(x);
                 case 'm': return __builtin_floor(x); case 'a': return __builtin_round(x); default: return __builtin_trunc(x); }
}
#define CVT_(NAME, T, LO, HI, FT) static inline u64 NAME(FT x, char r) { double d = rnd_((double)x, r); \
    if (d != d) return 0; if (d >= (HI)) return (u64)(T)((T)~(T)0 > 0 ? (T)~(T)0 : (T)(((u64)1 << (sizeof(T) * 8 - 1)) - 1)); \
    if (d <= (LO)) return (u64)(T)(LO); return (u64)(T)d; }
CVT_(cvts32_f, i32, -2147483648.0, 2147483647.0, float) CVT_(cvts32_d, i32, -2147483648.0, 2147483647.0, double)
CVT_(cvtu32_f, u32, 0.0, 4294967295.0, float) CVT_(cvtu32_d, u32, 0.0, 4294967295.0, double)
CVT_(cvts64_f, i64, -9223372036854775808.0, 9223372036854775807.0, float)
CVT_(cvts64_d, i64, -9223372036854775808.0, 9223372036854775807.0, double)
CVT_(cvtu64_f, u64, 0.0, 18446744073709551615.0, float) CVT_(cvtu64_d, u64, 0.0, 18446744073709551615.0, double)
