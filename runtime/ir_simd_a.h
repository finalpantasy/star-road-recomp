/* Scalar floating point and Advanced SIMD scalar helpers (extension "simd_a", task-5). Our own code.
 *
 * Included at the end of ir_ops.h.  Everything here follows the ARM Architecture Reference Manual pseudocode
 * (FPAdd, FPMul, FPMulAdd, FPRound, FPToFixed, FPRecipEstimate, ...) on raw bit patterns.
 *
 * Layout of each operation:
 *   - a fast path (always_inline, constants fold in lifted code) used only when FPCR == 0 and the operands are
 *     ordinary finite numbers; it computes the result with SSE/FMA and the cumulative flags with error-free
 *     transformations (exact IXC/UFC, no MXCSR access).  Host MXCSR must be at its default (round to nearest,
 *     no FTZ/DAZ).
 *   - a soft path (noinline, sa_s_*) implementing the full pseudocode with integer arithmetic: NaN rules, default
 *     NaN (FPCR.DN), flush to zero (FPCR.FZ, FZ16), rounding mode (FPCR.RMode), all cumulative flags.
 * Not handled: FPCR.AHP (alternative half precision), FPCR.AH / FIZ / NEP (FEAT_AFP), trapped exceptions.
 * Format codes (ftype): 0 single, 1 double, 3 half. */
#pragma once
#include <immintrin.h>

#define SA_AI static inline __attribute__((always_inline))
#define SA_NI static __attribute__((noinline))

#define SA_IOC 0x01u
#define SA_DZC 0x02u
#define SA_OFC 0x04u
#define SA_UFC 0x08u
#define SA_IXC 0x10u
#define SA_IDC 0x80u

/* ---------------------------------------------------------------- lane and bit helpers */
SA_AI u64 sa_get(int ft, v128 v) { return ft == 1 ? (u64)v[0] : ft == 0 ? (u64)(u32)v[0] : (u64)(u16)v[0]; }
SA_AI v128 sa_mk(u64 x) { v128 r = {(long long)x, 0}; return r; }
SA_AI float sa_f(u32 x) { float f; __builtin_memcpy(&f, &x, 4); return f; }
SA_AI u32 sa_fb(float f) { u32 x; __builtin_memcpy(&x, &f, 4); return x; }
SA_AI double sa_d(u64 x) { double f; __builtin_memcpy(&f, &x, 8); return f; }
SA_AI u64 sa_db(double f) { u64 x; __builtin_memcpy(&x, &f, 8); return x; }

SA_AI int sa_nbits(int ft) { return ft == 1 ? 64 : ft == 0 ? 32 : 16; }
SA_AI int sa_mbits(int ft) { return ft == 1 ? 52 : ft == 0 ? 23 : 10; }
SA_AI int sa_ebits(int ft) { return ft == 1 ? 11 : ft == 0 ? 8 : 5; }
SA_AI u64 sa_signbit(int ft) { return 1ull << (sa_nbits(ft) - 1); }
SA_AI u64 sa_dnan(int ft) { return ft == 1 ? 0x7FF8000000000000ull : ft == 0 ? 0x7FC00000ull : 0x7E00ull; }
SA_AI u64 sa_inf(int ft, int sign) { return ((u64)sign << (sa_nbits(ft) - 1)) | ((((u64)1 << sa_ebits(ft)) - 1) << sa_mbits(ft)); }
SA_AI u64 sa_zero(int ft, int sign) { return (u64)sign << (sa_nbits(ft) - 1); }
SA_AI u64 sa_maxfin(int ft, int sign) {
    return ((u64)sign << (sa_nbits(ft) - 1)) | ((((u64)1 << sa_ebits(ft)) - 2) << sa_mbits(ft)) | ((1ull << sa_mbits(ft)) - 1);
}
SA_AI int sa_rmode(Ctx *c) { return (c->fpcr >> 22) & 3; }
SA_AI int sa_msb128(u128 m) { u64 hi = (u64)(m >> 64); return hi ? 127 - __builtin_clzll(hi) : 63 - __builtin_clzll((u64)m); }

/* ---------------------------------------------------------------- unpack */
typedef struct { int cls, sign; s32 e; u64 m; } sa_u;   /* value = m * 2^e */
enum { SA_NUM = 0, SA_ZERO, SA_INF, SA_QNAN, SA_SNAN };

SA_NI sa_u sa_unpack(Ctx *c, int ft, u64 x) {
    int mb = sa_mbits(ft), eb = sa_ebits(ft), nb = sa_nbits(ft);
    sa_u u;
    u.sign = (int)((x >> (nb - 1)) & 1);
    u64 fr = x & ((1ull << mb) - 1);
    u32 ex = (u32)((x >> mb) & ((1u << eb) - 1));
    u.m = 0; u.e = 0; u.cls = SA_ZERO;
    if (ex == (1u << eb) - 1) { u.cls = fr == 0 ? SA_INF : (((fr >> (mb - 1)) & 1) ? SA_QNAN : SA_SNAN); return u; }
    int bias = (1 << (eb - 1)) - 1;
    if (ex == 0) {
        if (fr == 0) return u;
        int fz = ft == 3 ? (int)((c->fpcr >> 19) & 1) : (int)((c->fpcr >> 24) & 1);
        if (fz) { c->fpsr |= SA_IDC; return u; }
        u.cls = SA_NUM; u.m = fr; u.e = 1 - bias - mb; return u;
    }
    u.cls = SA_NUM; u.m = fr | (1ull << mb); u.e = (s32)ex - bias - mb;
    return u;
}

/* ---------------------------------------------------------------- rounding (FPRound) */
/* value = (m + eps) * 2^e with eps in (0,1) when sticky; m != 0. Uses FPCR RMode and FZ. */
SA_NI u64 sa_round(Ctx *c, int ft, int sign, u128 m, s32 e, int sticky) {
    int mb = sa_mbits(ft), eb = sa_ebits(ft), nb = sa_nbits(ft);
    int bias = (1 << (eb - 1)) - 1;
    int rm = sa_rmode(c);
    u64 sgn = (u64)sign << (nb - 1);
    int msb = sa_msb128(m);
    s32 E = e + msb;
    s32 emin = 1 - bias;
    int tiny = E < emin;
    int fz = ft == 3 ? (int)((c->fpcr >> 19) & 1) : (int)((c->fpcr >> 24) & 1);
    if (tiny && fz) { c->fpsr |= SA_UFC; return sgn; }
    s32 L = tiny ? emin - mb : E - mb;
    s32 sh = L - e;
    u128 kept;
    int g = 0, low = sticky;
    if (sh <= 0) kept = m << (-sh);
    else if (sh >= 129) { kept = 0; low |= m != 0; }
    else {
        kept = sh == 128 ? (u128)0 : (m >> sh);
        g = (int)((m >> (sh - 1)) & 1);
        low |= (m & (((u128)1 << (sh - 1)) - 1)) != 0;
    }
    int inexact = g | low, up = 0;
    switch (rm) {
    case 0: up = g && (low || ((u64)kept & 1)); break;
    case 1: up = !sign && inexact; break;
    case 2: up = sign && inexact; break;
    default: up = 0; break;
    }
    if (tiny && inexact) c->fpsr |= SA_UFC;
    u64 k = (u64)kept + (u64)up;
    int ovf = 0;
    u64 bits = 0;
    if (!tiny) {
        if (E > bias) ovf = 1;
        else {
            bits = ((u64)(E + bias) << mb) + (k - (1ull << mb));
            if (bits >= ((((u64)1 << eb) - 1) << mb)) ovf = 1;
        }
    } else bits = k;
    if (ovf) {
        c->fpsr |= SA_OFC | SA_IXC;
        int toinf = rm == 0 || (rm == 1 && !sign) || (rm == 2 && sign);
        return toinf ? sa_inf(ft, sign) : sa_maxfin(ft, sign);
    }
    if (inexact) c->fpsr |= SA_IXC;
    return sgn | bits;
}

/* ---------------------------------------------------------------- NaN rules */
SA_NI u64 sa_pnan(Ctx *c, int ft, u64 x, int cls) {
    if (cls == SA_SNAN) { c->fpsr |= SA_IOC; x |= 1ull << (sa_mbits(ft) - 1); }
    if ((c->fpcr >> 25) & 1) x = sa_dnan(ft);
    return x;
}
SA_NI int sa_nan2(Ctx *c, int ft, const sa_u *A, const sa_u *B, u64 a, u64 b, u64 *r) {
    if (A->cls == SA_SNAN) *r = sa_pnan(c, ft, a, SA_SNAN);
    else if (B->cls == SA_SNAN) *r = sa_pnan(c, ft, b, SA_SNAN);
    else if (A->cls == SA_QNAN) *r = sa_pnan(c, ft, a, SA_QNAN);
    else if (B->cls == SA_QNAN) *r = sa_pnan(c, ft, b, SA_QNAN);
    else return 0;
    return 1;
}
SA_NI int sa_nan3(Ctx *c, int ft, const sa_u *A, const sa_u *B, const sa_u *C, u64 a, u64 b, u64 cc, u64 *r) {
    if (A->cls == SA_SNAN) *r = sa_pnan(c, ft, a, SA_SNAN);
    else if (B->cls == SA_SNAN) *r = sa_pnan(c, ft, b, SA_SNAN);
    else if (C->cls == SA_SNAN) *r = sa_pnan(c, ft, cc, SA_SNAN);
    else if (A->cls == SA_QNAN) *r = sa_pnan(c, ft, a, SA_QNAN);
    else if (B->cls == SA_QNAN) *r = sa_pnan(c, ft, b, SA_QNAN);
    else if (C->cls == SA_QNAN) *r = sa_pnan(c, ft, cc, SA_QNAN);
    else return 0;
    return 1;
}
SA_NI u64 sa_invalid(Ctx *c, int ft) { c->fpsr |= SA_IOC; return sa_dnan(ft); }

/* ---------------------------------------------------------------- exact sum of two terms */
SA_NI u64 sa_sum(Ctx *c, int ft, int sa, u128 ma, s32 ea, int sb, u128 mb, s32 eb) {
    if (!ma && !mb) return sa_zero(ft, sa == sb ? sa : sa_rmode(c) == 2);
    if (!ma) return sa_round(c, ft, sb, mb, eb, 0);
    if (!mb) return sa_round(c, ft, sa, ma, ea, 0);
    int la = sa_msb128(ma), lb = sa_msb128(mb);
    if (eb + lb > ea + la) { u128 tm = ma; ma = mb; mb = tm; s32 te = ea; ea = eb; eb = te; int ts = sa; sa = sb; sb = ts; int tl = la; la = lb; lb = tl; }
    int up = 125 - la;
    u128 A = ma << up;
    s32 e0 = ea - up;
    s32 d = eb - e0;
    u128 B;
    int st = 0;
    if (d >= 0) B = mb << d;
    else {
        int r = -d;
        if (r >= 128) { B = 0; st = 1; }
        else { B = mb >> r; st = (mb & (((u128)1 << r) - 1)) != 0; }
    }
    u128 m;
    int sign = sa;
    if (sa == sb) m = A + B;
    else if (A >= B) { m = A - B - (u128)st; }
    else { m = B - A; sign = sb; }
    if (m == 0 && !st) return sa_zero(ft, sa_rmode(c) == 2);
    return sa_round(c, ft, sign, m, e0, st);
}

SA_NI u64 sa_s_addsub(Ctx *c, int ft, u64 a, u64 b, int sub) {
    sa_u A = sa_unpack(c, ft, a), B = sa_unpack(c, ft, b);
    u64 r;
    if (sa_nan2(c, ft, &A, &B, a, b, &r)) return r;
    if (sub) B.sign ^= 1;
    int ia = A.cls == SA_INF, ib = B.cls == SA_INF;
    if (ia && ib && A.sign != B.sign) return sa_invalid(c, ft);
    if (ia) return sa_inf(ft, A.sign);
    if (ib) return sa_inf(ft, B.sign);
    return sa_sum(c, ft, A.sign, A.m, A.e, B.sign, B.m, B.e);
}

SA_NI u64 sa_s_mul(Ctx *c, int ft, u64 a, u64 b, int mulx) {
    sa_u A = sa_unpack(c, ft, a), B = sa_unpack(c, ft, b);
    u64 r;
    if (sa_nan2(c, ft, &A, &B, a, b, &r)) return r;
    int ia = A.cls == SA_INF, ib = B.cls == SA_INF, za = A.cls == SA_ZERO, zb = B.cls == SA_ZERO;
    int sg = A.sign ^ B.sign;
    if ((ia && zb) || (za && ib)) {
        if (mulx) return sa_zero(ft, sg) | (ft == 1 ? 0x4000000000000000ull : ft == 0 ? 0x40000000ull : 0x4000ull);
        return sa_invalid(c, ft);
    }
    if (ia || ib) return sa_inf(ft, sg);
    if (za || zb) return sa_zero(ft, sg);
    return sa_round(c, ft, sg, (u128)A.m * B.m, A.e + B.e, 0);
}

SA_NI u64 sa_s_div(Ctx *c, int ft, u64 a, u64 b) {
    sa_u A = sa_unpack(c, ft, a), B = sa_unpack(c, ft, b);
    u64 r;
    if (sa_nan2(c, ft, &A, &B, a, b, &r)) return r;
    int ia = A.cls == SA_INF, ib = B.cls == SA_INF, za = A.cls == SA_ZERO, zb = B.cls == SA_ZERO;
    int sg = A.sign ^ B.sign;
    if ((ia && ib) || (za && zb)) return sa_invalid(c, ft);
    if (ia || zb) { if (!ia) c->fpsr |= SA_DZC; return sa_inf(ft, sg); }
    if (za || ib) return sa_zero(ft, sg);
    int la = 63 - __builtin_clzll(A.m), lb = 63 - __builtin_clzll(B.m);
    u64 am = A.m << (52 - la), bm = B.m << (52 - lb);
    s32 ea = A.e - (52 - la), eb = B.e - (52 - lb);
    u128 n = (u128)am << 62;
    u128 q = n / bm;
    int st = (n - q * bm) != 0;
    return sa_round(c, ft, sg, q, ea - 62 - eb, st);
}

SA_NI u64 sa_s_sqrt(Ctx *c, int ft, u64 a) {
    sa_u A = sa_unpack(c, ft, a);
    if (A.cls >= SA_QNAN) return sa_pnan(c, ft, a, A.cls);
    if (A.cls == SA_ZERO) return sa_zero(ft, A.sign);
    if (A.sign) return sa_invalid(c, ft);
    if (A.cls == SA_INF) return sa_inf(ft, 0);
    u128 m = A.m;
    s32 e = A.e;
    int sh = 110 - sa_msb128(m);
    if ((e - sh) & 1) sh++;
    m <<= sh; e -= sh;
    /* integer square root of m (bitwise) */
    u128 rem = m, res = 0, bit = (u128)1 << 126;
    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= res + bit) { rem -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return sa_round(c, ft, 0, res, e / 2, rem != 0);
}

SA_NI u64 sa_s_fma(Ctx *c, int ft, u64 a, u64 x, u64 y) {   /* a + x*y, FPMulAdd */
    sa_u A = sa_unpack(c, ft, a), X = sa_unpack(c, ft, x), Y = sa_unpack(c, ft, y);
    u64 r;
    int ix = X.cls == SA_INF, iy = Y.cls == SA_INF, zx = X.cls == SA_ZERO, zy = Y.cls == SA_ZERO;
    if (A.cls == SA_QNAN && ((ix && zy) || (zx && iy))) return sa_invalid(c, ft);
    if (sa_nan3(c, ft, &A, &X, &Y, a, x, y, &r)) return r;
    int ia = A.cls == SA_INF, ip = ix || iy, za = A.cls == SA_ZERO, zp = zx || zy;
    int sp = X.sign ^ Y.sign;
    if ((ix && zy) || (zx && iy) || (ia && ip && A.sign != sp)) return sa_invalid(c, ft);
    if ((ia && !A.sign) || (ip && !sp)) return sa_inf(ft, 0);
    if ((ia && A.sign) || (ip && sp)) return sa_inf(ft, 1);
    if (za && zp && A.sign == sp) return sa_zero(ft, A.sign);
    u128 pm = zp ? (u128)0 : (u128)X.m * Y.m;
    return sa_sum(c, ft, A.sign, A.m, A.e, sp, pm, X.e + Y.e);
}

/* ---------------------------------------------------------------- compare, min/max */
SA_AI int sa_sgn(const sa_u *u) { return u->cls == SA_ZERO ? 0 : (u->sign ? -1 : 1); }
SA_NI int sa_cmpv(const sa_u *A, const sa_u *B) {
    int sa = sa_sgn(A), sb = sa_sgn(B);
    if (sa != sb) return sa < sb ? -1 : 1;
    if (sa == 0) return 0;
    int mag;
    if (A->cls == SA_INF || B->cls == SA_INF) mag = (A->cls == SA_INF) - (B->cls == SA_INF);
    else {
        int la = 63 - __builtin_clzll(A->m), lb = 63 - __builtin_clzll(B->m);
        s32 ta = A->e + la, tb = B->e + lb;
        if (ta != tb) mag = ta < tb ? -1 : 1;
        else { u64 ma = A->m << (63 - la), mb = B->m << (63 - lb); mag = ma == mb ? 0 : ma < mb ? -1 : 1; }
    }
    return sa < 0 ? -mag : mag;
}

SA_NI int sa_s_fcmp(Ctx *c, int ft, u64 a, u64 b, int e) {    /* returns N Z C V as bits 3..0 */
    sa_u A = sa_unpack(c, ft, a), B = sa_unpack(c, ft, b);
    if (A.cls >= SA_QNAN || B.cls >= SA_QNAN) {
        if (e || A.cls == SA_SNAN || B.cls == SA_SNAN) c->fpsr |= SA_IOC;
        return 3;
    }
    int k = sa_cmpv(&A, &B);
    return k == 0 ? 6 : k < 0 ? 8 : 2;
}

SA_NI u64 sa_s_minmax(Ctx *c, int ft, u64 a, u64 b, int ismin, int isnum) {
    sa_u A = sa_unpack(c, ft, a), B = sa_unpack(c, ft, b);
    if (isnum) {
        int an = A.cls >= SA_QNAN, bn = B.cls >= SA_QNAN;
        if (!(an && bn)) {
            if (A.cls == SA_QNAN) { a = sa_inf(ft, !ismin); A = sa_unpack(c, ft, a); }
            else if (B.cls == SA_QNAN) { b = sa_inf(ft, !ismin); B = sa_unpack(c, ft, b); }
        }
    }
    u64 r;
    if (sa_nan2(c, ft, &A, &B, a, b, &r)) return r;
    int k = sa_cmpv(&A, &B);
    int pick1 = ismin ? k < 0 : k > 0;
    const sa_u *S = pick1 ? &A : &B;
    u64 sx = pick1 ? a : b;
    if (S->cls == SA_ZERO) return sa_zero(ft, ismin ? (A.sign | B.sign) : (A.sign & B.sign));
    return sx;
}

/* ---------------------------------------------------------------- format conversion, round to integral */
SA_NI u64 sa_s_cvt(Ctx *c, int sft, int dft, u64 a) {
    sa_u A = sa_unpack(c, sft, a);
    int dn = (c->fpcr >> 25) & 1;
    if (A.cls >= SA_QNAN) {
        if (A.cls == SA_SNAN) c->fpsr |= SA_IOC;
        if (dn) return sa_dnan(dft);
        int smb = sa_mbits(sft), dmb = sa_mbits(dft);
        u64 fr = a & ((1ull << smb) - 1);
        fr = dmb > smb ? fr << (dmb - smb) : fr >> (smb - dmb);
        fr |= 1ull << (dmb - 1);
        return sa_inf(dft, A.sign) | fr;
    }
    if (A.cls == SA_INF) return sa_inf(dft, A.sign);
    if (A.cls == SA_ZERO) return sa_zero(dft, A.sign);
    return sa_round(c, dft, A.sign, A.m, A.e, 0);
}

SA_NI u64 sa_s_rint(Ctx *c, int ft, u64 x, int rm, int exact) {   /* rm: 0 N, 1 P, 2 M, 3 Z, 4 A */
    sa_u U = sa_unpack(c, ft, x);
    if (U.cls >= SA_QNAN) return sa_pnan(c, ft, x, U.cls);
    if (U.cls == SA_INF) return x;
    if (U.cls == SA_ZERO) return sa_zero(ft, U.sign);
    if (U.e >= 0) return x;
    int sh = -U.e;
    u64 ip, g = 0, low = 0;
    if (sh >= 64) { ip = 0; low = 1; }
    else { ip = U.m >> sh; g = (U.m >> (sh - 1)) & 1; low = (U.m & ((1ull << (sh - 1)) - 1)) != 0; }
    int inex = (int)(g | low), up = 0;
    switch (rm) {
    case 0: up = g && (low || (ip & 1)); break;
    case 1: up = !U.sign && inex; break;
    case 2: up = U.sign && inex; break;
    case 4: up = (int)g; break;
    default: break;
    }
    ip += (u64)up;
    if (exact && inex) c->fpsr |= SA_IXC;
    if (ip == 0) return sa_zero(ft, U.sign);
    return sa_round(c, ft, U.sign, ip, 0, 0);
}

/* ---------------------------------------------------------------- to and from integers */
SA_NI u64 sa_s_tofix(Ctx *c, int ft, u64 x, int sf, int uns, int fbits, int rm) {
    sa_u U = sa_unpack(c, ft, x);
    int nb = sf ? 64 : 32;
    u64 mask = sf ? ~0ull : 0xFFFFFFFFull;
    if (U.cls == SA_QNAN || U.cls == SA_SNAN) { c->fpsr |= SA_IOC; return 0; }
    if (U.cls == SA_ZERO) return 0;
    u128 ip = 0;
    int g = 0, low = 0, ovf = 0;
    if (U.cls == SA_INF) ovf = 1;
    else {
        s32 e = U.e + fbits;
        if (e >= 0) { if (e > 70) ovf = 1; else ip = (u128)U.m << e; }
        else {
            int sh = -e;
            if (sh >= 64) { low = 1; }
            else { ip = U.m >> sh; g = (int)((U.m >> (sh - 1)) & 1); low = (U.m & ((1ull << (sh - 1)) - 1)) != 0; }
        }
    }
    int inex = g | low;
    if (!ovf) {
        int up = 0;
        switch (rm) {
        case 0: up = g && (low || ((u64)ip & 1)); break;
        case 1: up = !U.sign && inex; break;
        case 2: up = U.sign && inex; break;
        case 4: up = g; break;
        default: break;
        }
        ip += (u128)up;
        if (uns) { if (U.sign ? ip != 0 : ip > (u128)mask) ovf = 1; }
        else { u128 lim = (u128)1 << (nb - 1); if (U.sign ? ip > lim : ip > lim - 1) ovf = 1; }
    }
    if (ovf) {
        c->fpsr |= SA_IOC;
        if (uns) return U.sign ? 0 : mask;
        return U.sign ? ((u64)1 << (nb - 1)) : (((u64)1 << (nb - 1)) - 1);
    }
    if (inex) c->fpsr |= SA_IXC;
    u64 r = (u64)ip;
    if (U.sign) r = (u64)0 - r;
    return r & mask;
}

SA_NI u64 sa_s_fromfix(Ctx *c, int ft, u64 v, int sf, int uns, int fbits) {
    int sign = 0;
    u64 mag = sf ? v : (u32)v;
    if (!uns) {
        if (sf ? (s64)v < 0 : (s32)v < 0) { sign = 1; mag = sf ? (u64)0 - v : (u64)(u32)(0u - (u32)v); }
    }
    if (mag == 0) return 0;
    return sa_round(c, ft, sign, mag, -fbits, 0);
}

/* ---------------------------------------------------------------- estimates */
SA_AI u32 sa_recip_est(u32 a) { a = a * 2 + 1; u32 b = (1u << 19) / a; return (b + 1) / 2; }
SA_AI u32 sa_rsqrt_est(u32 a) {
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    u64 b = 512;
    while ((u64)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (u32)((b + 1) / 2);
}

SA_NI u64 sa_s_recpe(Ctx *c, int ft, u64 x) {
    int mb = sa_mbits(ft), eb = sa_ebits(ft), nb = sa_nbits(ft);
    sa_u U = sa_unpack(c, ft, x);
    if (U.cls >= SA_QNAN) return sa_pnan(c, ft, x, U.cls);
    int sign = U.sign;
    if (U.cls == SA_INF) return sa_zero(ft, sign);
    if (U.cls == SA_ZERO) { c->fpsr |= SA_DZC; return sa_inf(ft, sign); }
    int T = U.e + sa_msb128(U.m);
    int half = 1 << (eb - 1);
    int rm = sa_rmode(c);
    if (T < -half) {
        c->fpsr |= SA_OFC | SA_IXC;
        int toinf = rm == 0 || (rm == 1 && !sign) || (rm == 2 && sign);
        return toinf ? sa_inf(ft, sign) : sa_maxfin(ft, sign);
    }
    int fz = ft == 3 ? (int)((c->fpcr >> 19) & 1) : (int)((c->fpcr >> 24) & 1);
    if (fz && T >= half - 2) { c->fpsr |= SA_UFC; return sa_zero(ft, sign); }
    u64 fr = x & ((1ull << mb) - 1);
    s32 ex = (s32)((x >> mb) & ((1u << eb) - 1));
    if (ex == 0) {
        if (((fr >> (mb - 1)) & 1) == 0) { ex = -1; fr = (fr << 2) & ((1ull << mb) - 1); }
        else fr = (fr << 1) & ((1ull << mb) - 1);
    }
    u32 scaled = 256 | (u32)((fr >> (mb - 8)) & 0xFF);
    s32 rexp = (2 * ((1 << (eb - 1)) - 1) - 1) - ex;
    u32 est = sa_recip_est(scaled);
    u64 fraction = (u64)(est & 0xFF) << (mb - 8);
    if (rexp == 0) fraction = (1ull << (mb - 1)) | (fraction >> 1);
    else if (rexp == -1) { fraction = (1ull << (mb - 2)) | (fraction >> 2); rexp = 0; }
    return ((u64)sign << (nb - 1)) | ((u64)rexp << mb) | fraction;
}

SA_NI u64 sa_s_rsqrte(Ctx *c, int ft, u64 x) {
    int mb = sa_mbits(ft), eb = sa_ebits(ft);
    sa_u U = sa_unpack(c, ft, x);
    if (U.cls >= SA_QNAN) return sa_pnan(c, ft, x, U.cls);
    if (U.cls == SA_ZERO) { c->fpsr |= SA_DZC; return sa_inf(ft, U.sign); }
    if (U.sign) return sa_invalid(c, ft);
    if (U.cls == SA_INF) return sa_zero(ft, 0);
    u64 fr = x & ((1ull << mb) - 1);
    s32 ex = (s32)((x >> mb) & ((1u << eb) - 1));
    u64 fm = (1ull << mb) - 1;
    if (ex == 0) {
        while (((fr >> (mb - 1)) & 1) == 0) { fr = (fr << 1) & fm; ex--; }
        fr = (fr << 1) & fm;
    }
    u32 scaled;
    if ((ex & 1) == 0) scaled = 256 | (u32)((fr >> (mb - 8)) & 0xFF);
    else scaled = 128 | (u32)((fr >> (mb - 7)) & 0x7F);
    s32 num = 3 * (1 << (eb - 1)) - 4 - ex;
    s32 rexp = num >= 0 ? num / 2 : -((-num + 1) / 2);
    u32 est = sa_rsqrt_est(scaled);
    return ((u64)rexp << mb) | ((u64)(est & 0xFF) << (mb - 8));
}

SA_NI u64 sa_s_recpx(Ctx *c, int ft, u64 x) {
    int mb = sa_mbits(ft), eb = sa_ebits(ft), nb = sa_nbits(ft);
    sa_u U = sa_unpack(c, ft, x);
    if (U.cls >= SA_QNAN) return sa_pnan(c, ft, x, U.cls);
    u64 ex = (x >> mb) & ((1u << eb) - 1);
    u64 r = (ex == 0) ? (((u64)1 << eb) - 2) : (~ex & (((u64)1 << eb) - 1));
    return (x & ((u64)1 << (nb - 1))) | (r << mb);
}

/* FRECPS / FRSQRTS: 2 + x*y and (3 + x*y)/2 with x pre-negated, fused; an exact zero result is +0 */
SA_NI u64 sa_s_step(Ctx *c, int ft, u64 x, u64 y, int rsqrt) {
    int nb = sa_nbits(ft), mb = sa_mbits(ft), eb = sa_ebits(ft);
    x ^= 1ull << (nb - 1);
    sa_u X = sa_unpack(c, ft, x), Y = sa_unpack(c, ft, y);
    u64 r;
    if (sa_nan2(c, ft, &X, &Y, x, y, &r)) return r;
    int ix = X.cls == SA_INF, iy = Y.cls == SA_INF, zx = X.cls == SA_ZERO, zy = Y.cls == SA_ZERO;
    int sp = X.sign ^ Y.sign;
    /* constant: 1.5 (rsqrt) or 2.0 as m * 2^e */
    u64 km = rsqrt ? 3 : 1;
    s32 ke = rsqrt ? -1 : 1;
    u64 cst = rsqrt ? ((((u64)1 << (eb - 1)) - 1) << mb) | (1ull << (mb - 1)) : (((u64)1 << (eb - 1)) << mb);
    if ((ix && zy) || (zx && iy)) return cst;
    if (ix || iy) return sa_inf(ft, sp);
    u128 pm = (zx || zy) ? (u128)0 : (u128)X.m * Y.m;
    s32 pe = X.e + Y.e - (rsqrt ? 1 : 0);
    u64 res = sa_sum(c, ft, 0, km, ke, sp, pm, pe);
    if ((res & ~sa_signbit(ft)) == 0) return 0;
    return res;
}

/* ================================================================== fast paths */
#define SA_E32(x) (((x) & 0x7F800000u) == 0x7F800000u)
#define SA_E64(x) (((x) & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)

/* two-operand arithmetic; kinds: 0 fmul 1 fdiv 2 fadd 3 fsub 4 fmax 5 fmin 6 fmaxnm 7 fminnm 8 fnmul 9 fabd 10 fmulx */
SA_AI int sa_fast2s(Ctx *c, int kind, u32 a, u32 b, u64 *res) {
    if (SA_E32(a) || SA_E32(b)) return 0;
    float x = sa_f(a), y = sa_f(b), r;
    u32 fx = 0;
    switch (kind) {
    case 2: case 3: case 9: {
        if (kind != 2) y = -y;
        r = x + y;
        if (SA_E32(sa_fb(r))) return 0;
        float bb = r - x;
        float err = (x - (r - bb)) + (y - bb);
        if (err != 0) fx |= SA_IXC;
        if (kind == 9) r = sa_f(sa_fb(r) & 0x7FFFFFFFu);
        break; }
    case 0: case 8: case 10: {
        double p = (double)x * (double)y;
        r = (float)p;
        if (SA_E32(sa_fb(r))) return 0;
        if ((double)r != p) { fx |= SA_IXC; if (__builtin_fabs(p) < 0x1p-126) fx |= SA_UFC; }
        if (kind == 8) r = sa_f(sa_fb(r) ^ 0x80000000u);
        break; }
    case 1: {
        if (y == 0) return 0;
        r = x / y;
        if (SA_E32(sa_fb(r))) return 0;
        double rem = (double)x - (double)r * (double)y;
        if (rem != 0) { fx |= SA_IXC; if (__builtin_fabs((double)x) < 0x1p-126 * __builtin_fabs((double)y)) fx |= SA_UFC; }
        break; }
    default: {
        int ismin = kind == 5 || kind == 7;
        if (x > y) *res = ismin ? b : a;
        else if (y > x) *res = ismin ? a : b;
        else *res = x == 0 ? (ismin ? (a | b) : (a & b)) : a;
        return 1; }
    }
    if (fx) c->fpsr |= fx;
    *res = sa_fb(r);
    return 1;
}

SA_AI int sa_fast2d(Ctx *c, int kind, u64 a, u64 b, u64 *res) {
    if (SA_E64(a) || SA_E64(b)) return 0;
    double x = sa_d(a), y = sa_d(b), r;
    u32 fx = 0;
    switch (kind) {
    case 2: case 3: case 9: {
        if (kind != 2) y = -y;
        r = x + y;
        if (SA_E64(sa_db(r))) return 0;
        double bb = r - x;
        double err = (x - (r - bb)) + (y - bb);
        if (err != 0) fx |= SA_IXC;
        if (kind == 9) r = sa_d(sa_db(r) & 0x7FFFFFFFFFFFFFFFull);
        break; }
    case 0: case 8: case 10: {
        r = x * y;
        if (SA_E64(sa_db(r))) return 0;
        if (r == 0) { if (x != 0 && y != 0) return 0; }
        else {
            if (__builtin_fabs(r) < 0x1p-900) return 0;
            if (__builtin_fma(x, y, -r) != 0) fx |= SA_IXC;
        }
        if (kind == 8) r = sa_d(sa_db(r) ^ 0x8000000000000000ull);
        break; }
    case 1: {
        if (y == 0) return 0;
        r = x / y;
        if (SA_E64(sa_db(r))) return 0;
        if (x != 0) {
            if (__builtin_fabs(x) < 0x1p-900 || __builtin_fabs(r) < 0x1p-900) return 0;
            if (__builtin_fma(-r, y, x) != 0) fx |= SA_IXC;
        }
        break; }
    default: {
        int ismin = kind == 5 || kind == 7;
        if (x > y) *res = ismin ? b : a;
        else if (y > x) *res = ismin ? a : b;
        else *res = x == 0 ? (ismin ? (a | b) : (a & b)) : a;
        return 1; }
    }
    if (fx) c->fpsr |= fx;
    *res = sa_db(r);
    return 1;
}

SA_NI u64 sa_s_fp2(Ctx *c, int ft, int kind, u64 a, u64 b) {
    switch (kind) {
    case 0: return sa_s_mul(c, ft, a, b, 0);
    case 1: return sa_s_div(c, ft, a, b);
    case 2: return sa_s_addsub(c, ft, a, b, 0);
    case 3: return sa_s_addsub(c, ft, a, b, 1);
    case 4: return sa_s_minmax(c, ft, a, b, 0, 0);
    case 5: return sa_s_minmax(c, ft, a, b, 1, 0);
    case 6: return sa_s_minmax(c, ft, a, b, 0, 1);
    case 7: return sa_s_minmax(c, ft, a, b, 1, 1);
    case 8: return sa_s_mul(c, ft, a, b, 0) ^ sa_signbit(ft);
    case 9: return sa_s_addsub(c, ft, a, b, 1) & ~sa_signbit(ft);
    default: return sa_s_mul(c, ft, a, b, 1);
    }
}

SA_AI v128 sa_fp2(Ctx *c, int ft, int kind, v128 vn, v128 vm) {
    u64 a = sa_get(ft, vn), b = sa_get(ft, vm), r;
    if (ft == 0) { if (c->fpcr == 0 && sa_fast2s(c, kind, (u32)a, (u32)b, &r)) return sa_mk(r); }
    else if (ft == 1) { if (c->fpcr == 0 && sa_fast2d(c, kind, a, b, &r)) return sa_mk(r); }
    return sa_mk(sa_s_fp2(c, ft, kind, a, b));
}

/* three operand fused: kinds 0 fmadd 1 fmsub 2 fnmadd 3 fnmsub; d = a +- n*m */
SA_AI int sa_fast3s(Ctx *c, u32 a, u32 x, u32 y, u64 *res) {
    if (SA_E32(a) || SA_E32(x) || SA_E32(y)) return 0;
    float fx_ = sa_f(x), fy = sa_f(y), fa = sa_f(a);
    double p = (double)fx_ * (double)fy;
    double za = (double)fa;
    double s = p + za;
    double bb = s - p;
    double e = (p - (s - bb)) + (za - bb);
    float r = __builtin_fmaf(fx_, fy, fa);
    if (SA_E32(sa_fb(r))) return 0;
    if (!(s == 0 && e == 0) && __builtin_fabs(s) < 0x1p-125) return 0;
    if (e != 0 || s != (double)r) c->fpsr |= SA_IXC;
    *res = sa_fb(r);
    return 1;
}

SA_AI v128 sa_fp3(Ctx *c, int ft, int kind, v128 vn, v128 vm, v128 va) {
    u64 x = sa_get(ft, vn), y = sa_get(ft, vm), a = sa_get(ft, va), r;
    u64 sb = sa_signbit(ft);
    if (kind == 1 || kind == 2) x ^= sb;
    if (kind >= 2) a ^= sb;
    if (ft == 0 && c->fpcr == 0 && sa_fast3s(c, (u32)a, (u32)x, (u32)y, &r)) return sa_mk(r);
    return sa_mk(sa_s_fma(c, ft, a, x, y));
}

/* one-source: kinds are the opcode field: 0 fmov 1 fabs 2 fneg 3 fsqrt 4 fcvt->S 5 fcvt->D 7 fcvt->H 8 frintn 9 frintp 10 frintm
 * 11 frintz 12 frinta 14 frintx 15 frinti */
SA_AI u64 sa_fsqrt(Ctx *c, int ft, u64 a) {
    if (c->fpcr == 0) {
        if (ft == 0) {
            u32 u = (u32)a;
            if (!SA_E32(u) && (u == 0 || u == 0x80000000u || !(u >> 31))) {
                float x = sa_f(u);
                float r = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
                if ((double)r * (double)r != (double)x) c->fpsr |= SA_IXC;
                return sa_fb(r);
            }
        } else if (ft == 1) {
            if (!SA_E64(a) && (a == 0 || a == 0x8000000000000000ull || (!(a >> 63) && a >= 0x07B0000000000000ull))) {
                double x = sa_d(a);
                double r = _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
                if (x != 0 && __builtin_fma(r, r, -x) != 0) c->fpsr |= SA_IXC;
                return sa_db(r);
            }
        }
    }
    return sa_s_sqrt(c, ft, a);
}

SA_AI float sa_rndf(float x, int rm) {
    __m128 v = _mm_set_ss(x);
    switch (rm) {
    case 1: v = _mm_round_ss(v, v, 0xA); break;
    case 2: v = _mm_round_ss(v, v, 0x9); break;
    case 3: v = _mm_round_ss(v, v, 0xB); break;
    default: v = _mm_round_ss(v, v, 0x8); break;
    }
    return _mm_cvtss_f32(v);
}
SA_AI double sa_rndd(double x, int rm) {
    __m128d v = _mm_set_sd(x);
    switch (rm) {
    case 1: v = _mm_round_sd(v, v, 0xA); break;
    case 2: v = _mm_round_sd(v, v, 0x9); break;
    case 3: v = _mm_round_sd(v, v, 0xB); break;
    default: v = _mm_round_sd(v, v, 0x8); break;
    }
    return _mm_cvtsd_f64(v);
}

SA_AI u64 sa_frint(Ctx *c, int ft, u64 a, int rm, int exact) {   /* rm 0 N 1 P 2 M 3 Z 4 A */
    if (c->fpcr == 0) {
        if (ft == 0 && !SA_E32((u32)a)) {
            float x = sa_f((u32)a), r;
            if (rm == 4) { r = sa_rndf(x, 3); float d = x - r; if (__builtin_fabsf(d) >= 0.5f) r += x < 0 ? -1.0f : 1.0f; }
            else r = sa_rndf(x, rm);
            if (exact && r != x) c->fpsr |= SA_IXC;
            return sa_fb(r);
        }
        if (ft == 1 && !SA_E64(a)) {
            double x = sa_d(a), r;
            if (rm == 4) { r = sa_rndd(x, 3); double d = x - r; if (__builtin_fabs(d) >= 0.5) r += x < 0 ? -1.0 : 1.0; }
            else r = sa_rndd(x, rm);
            if (exact && r != x) c->fpsr |= SA_IXC;
            return sa_db(r);
        }
    }
    return sa_s_rint(c, ft, a, rm, exact);
}

SA_AI u64 sa_fcvt(Ctx *c, int sft, int dft, u64 a) {
    if (c->fpcr == 0) {
        if (sft == 0 && dft == 1 && !SA_E32((u32)a)) return sa_db((double)sa_f((u32)a));
        if (sft == 1 && dft == 0 && !SA_E64(a)) {
            double d = sa_d(a);
            float r = (float)d;
            if (SA_E32(sa_fb(r))) return sa_s_cvt(c, sft, dft, a);
            if ((double)r != d) { c->fpsr |= SA_IXC; if (__builtin_fabs(d) < 0x1p-126) c->fpsr |= SA_UFC; }
            return sa_fb(r);
        }
    }
    return sa_s_cvt(c, sft, dft, a);
}

SA_AI v128 sa_fp1(Ctx *c, int ft, int kind, v128 vn) {
    u64 a = sa_get(ft, vn), r;
    switch (kind) {
    case 0: r = a; break;
    case 1: r = a & ~sa_signbit(ft); break;
    case 2: r = a ^ sa_signbit(ft); break;
    case 3: r = sa_fsqrt(c, ft, a); break;
    case 4: r = sa_fcvt(c, ft, 0, a); break;
    case 5: r = sa_fcvt(c, ft, 1, a); break;
    case 7: r = sa_fcvt(c, ft, 3, a); break;
    case 8: r = sa_frint(c, ft, a, 0, 0); break;
    case 9: r = sa_frint(c, ft, a, 1, 0); break;
    case 10: r = sa_frint(c, ft, a, 2, 0); break;
    case 11: r = sa_frint(c, ft, a, 3, 0); break;
    case 12: r = sa_frint(c, ft, a, 4, 0); break;
    case 14: r = sa_frint(c, ft, a, sa_rmode(c), 1); break;
    default: r = sa_frint(c, ft, a, sa_rmode(c), 0); break;
    }
    return sa_mk(r);
}

/* estimates and steps (two-reg misc / three-same scalar forms): kinds 0 frecpe 1 frsqrte 2 frecpx */
SA_AI v128 sa_fest(Ctx *c, int ft, int kind, v128 vn) {
    u64 a = sa_get(ft, vn);
    return sa_mk(kind == 0 ? sa_s_recpe(c, ft, a) : kind == 1 ? sa_s_rsqrte(c, ft, a) : sa_s_recpx(c, ft, a));
}
SA_AI v128 sa_fstep(Ctx *c, int ft, int rsqrt, v128 vn, v128 vm) {
    return sa_mk(sa_s_step(c, ft, sa_get(ft, vn), sa_get(ft, vm), rsqrt));
}

/* ---------------------------------------------------------------- compare (FCMP/FCMPE/FCCMP) */
SA_AI void sa_fcmp(Ctx *c, int ft, int e, v128 vn, v128 vm, int zero, u8 *fl) {
    u64 a = sa_get(ft, vn), b = zero ? 0 : sa_get(ft, vm);
    int n;
    if (c->fpcr == 0 && ft == 0) {
        float x = sa_f((u32)a), y = sa_f((u32)b);
        int lt = x < y, eq = x == y, gt = x > y;
        if (lt | eq | gt) { fl[0] = lt; fl[1] = eq; fl[2] = !lt; fl[3] = 0; return; }
        n = sa_s_fcmp(c, ft, a, b, e);
    } else if (c->fpcr == 0 && ft == 1) {
        double x = sa_d(a), y = sa_d(b);
        int lt = x < y, eq = x == y, gt = x > y;
        if (lt | eq | gt) { fl[0] = lt; fl[1] = eq; fl[2] = !lt; fl[3] = 0; return; }
        n = sa_s_fcmp(c, ft, a, b, e);
    } else n = sa_s_fcmp(c, ft, a, b, e);
    fl[0] = (n >> 3) & 1; fl[1] = (n >> 2) & 1; fl[2] = (n >> 1) & 1; fl[3] = n & 1;
}

/* ---------------------------------------------------------------- float to integer and back */
SA_AI u64 sa_fcvti(Ctx *c, int ft, int sf, int rm, int uns, int fbits, v128 vn) {
    u64 a = sa_get(ft, vn);
    if (c->fpcr == 0 && rm != 4 && ft != 3) {
        double d = ft == 0 ? (double)sa_f((u32)a) : sa_d(a);
        if (d == d) {
            double s = d * sa_d((u64)(1023 + fbits) << 52);
            double r = sa_rndd(s, rm);
            int ok;
            if (uns) ok = r >= 0 && r < (sf ? 0x1p64 : 0x1p32);
            else ok = sf ? (r >= -0x1p63 && r < 0x1p63) : (r >= -0x1p31 && r < 0x1p31);
            if (ok) {
                u64 v;
                if (uns) v = r >= 0x1p63 ? (u64)(s64)(r - 0x1p63) + (1ull << 63) : (u64)(s64)r;
                else v = (u64)(s64)r;
                if (r != s) c->fpsr |= SA_IXC;
                return sf ? v : (u32)v;
            }
        }
    }
    return sa_s_tofix(c, ft, a, sf, uns, fbits, rm);
}

SA_AI v128 sa_cvtf(Ctx *c, int ft, int sf, int uns, int fbits, u64 v) {
    if (c->fpcr == 0 && ft != 3) {
        u64 mag = sf ? v : (u32)v;
        int neg = 0;
        if (!uns) { if (sf ? (s64)v < 0 : (s32)v < 0) { neg = 1; mag = sf ? (u64)0 - v : (u64)(u32)(0u - (u32)v); } }
        if (mag != 0) {
            int width = 64 - __builtin_clzll(mag) - __builtin_ctzll(mag);
            if (width > (ft == 0 ? 24 : 53)) c->fpsr |= SA_IXC;
        }
        if (ft == 0) {
            float r = (float)mag;
            if (neg) r = -r;
            if (fbits) r *= sa_f((u32)(127 - fbits) << 23);
            return sa_mk(sa_fb(r));
        } else {
            double r = (double)mag;
            if (neg) r = -r;
            if (fbits) r *= sa_d((u64)(1023 - fbits) << 52);
            return sa_mk(sa_db(r));
        }
    }
    return sa_mk(sa_s_fromfix(c, ft, v, sf, uns, fbits));
}

/* ---------------------------------------------------------------- moves */
SA_AI u64 sa_vx(int sz, int hi, v128 v) { return hi ? (u64)v[1] : sz == 8 ? (u64)v[0] : sz == 4 ? (u64)(u32)v[0] : (u64)(u16)v[0]; }
SA_AI v128 sa_xv(int sz, u64 x) { return sa_mk(sz == 8 ? x : sz == 4 ? (u64)(u32)x : (u64)(u16)x); }
SA_AI v128 sa_hi(v128 v, u64 x) { v[1] = (long long)x; return v; }
SA_AI v128 sa_dup(int sz, int idx, v128 v) {
    u64 r = 0;
    __builtin_memcpy(&r, (const u8 *)&v + idx * sz, sz);
    return sa_mk(r);
}
SA_AI u64 sa_lane(int ft, int idx, v128 v) {
    u64 r = 0;
    int sz = ft == 1 ? 8 : 4;
    __builtin_memcpy(&r, (const u8 *)&v + idx * sz, sz);
    return r;
}

/* scalar by element: kinds 0 fmla 1 fmls 2 fmul 3 fmulx; vd is the accumulator for fmla/fmls */
SA_AI v128 sa_felem(Ctx *c, int ft, int kind, v128 vd, v128 vn, v128 vm, int idx) {
    u64 x = sa_get(ft, vn), y = sa_lane(ft, idx, vm), r;
    if (kind >= 2) {
        if (kind == 2) { if (ft == 0 && c->fpcr == 0 && sa_fast2s(c, 0, (u32)x, (u32)y, &r)) return sa_mk(r);
                         if (ft == 1 && c->fpcr == 0 && sa_fast2d(c, 0, x, y, &r)) return sa_mk(r); }
        return sa_mk(sa_s_mul(c, ft, x, y, kind == 3));
    }
    if (kind == 1) x ^= sa_signbit(ft);
    u64 a = sa_get(ft, vd);
    if (ft == 0 && c->fpcr == 0 && sa_fast3s(c, (u32)a, (u32)x, (u32)y, &r)) return sa_mk(r);
    return sa_mk(sa_s_fma(c, ft, a, x, y));
}

/* scalar three-same FP (vector register form): kinds 0 fabd 1 fmulx 2 frecps 3 frsqrts 4 fcmeq 5 fcmge 6 fcmgt 7 facge 8 facgt */
SA_NI u64 sa_s_fcmpmask(Ctx *c, int ft, int kind, u64 a, u64 b) {
    u64 sb = ~sa_signbit(ft);
    if (kind == 7 || kind == 8) { a &= sb; b &= sb; }
    sa_u A = sa_unpack(c, ft, a), B = sa_unpack(c, ft, b);
    u64 ones = ft == 1 ? ~0ull : ft == 0 ? 0xFFFFFFFFull : 0xFFFFull;
    if (A.cls >= SA_QNAN || B.cls >= SA_QNAN) {
        if (kind != 4 || A.cls == SA_SNAN || B.cls == SA_SNAN) c->fpsr |= SA_IOC;
        return 0;
    }
    int k = sa_cmpv(&A, &B);
    int t = kind == 4 ? k == 0 : (kind == 5 || kind == 7) ? k >= 0 : k > 0;
    return t ? ones : 0;
}
SA_AI v128 sa_f3same(Ctx *c, int ft, int kind, v128 vn, v128 vm) {
    u64 a = sa_get(ft, vn), b = sa_get(ft, vm);
    switch (kind) {
    case 0: return sa_fp2(c, ft, 9, vn, vm);
    case 1: return sa_fp2(c, ft, 10, vn, vm);
    case 2: return sa_fstep(c, ft, 0, vn, vm);
    case 3: return sa_fstep(c, ft, 1, vn, vm);
    default: return sa_mk(sa_s_fcmpmask(c, ft, kind, a, b));
    }
}

/* scalar compare against zero (two-reg misc): kinds 0 fcmgt 1 fcmge 2 fcmeq 3 fcmle 4 fcmlt */
SA_NI u64 sa_s_fcmz(Ctx *c, int ft, int kind, u64 a) {
    sa_u A = sa_unpack(c, ft, a), Z = sa_unpack(c, ft, 0);
    u64 ones = ft == 1 ? ~0ull : ft == 0 ? 0xFFFFFFFFull : 0xFFFFull;
    if (A.cls >= SA_QNAN) { if (kind != 2 || A.cls == SA_SNAN) c->fpsr |= SA_IOC; return 0; }
    int k = sa_cmpv(&A, &Z);
    int t = kind == 0 ? k > 0 : kind == 1 ? k >= 0 : kind == 2 ? k == 0 : kind == 3 ? k <= 0 : k < 0;
    return t ? ones : 0;
}
SA_AI v128 sa_fcmz(Ctx *c, int ft, int kind, v128 vn) { return sa_mk(sa_s_fcmz(c, ft, kind, sa_get(ft, vn))); }

/* scalar pairwise: kinds 0 faddp 1 fmaxnmp 2 fmaxp 3 fminnmp 4 fminp; the pair is the two lanes of the source */
SA_AI v128 sa_fpair(Ctx *c, int ft, int kind, v128 vn) {
    u64 a, b;
    if (ft == 1) { a = (u64)vn[0]; b = (u64)vn[1]; } else { a = (u32)vn[0]; b = (u32)((u64)vn[0] >> 32); }
    v128 va = sa_mk(a), vb = sa_mk(b);
    int k = kind == 0 ? 2 : kind == 1 ? 6 : kind == 2 ? 4 : kind == 3 ? 7 : 5;
    return sa_fp2(c, ft, k, va, vb);
}
SA_AI v128 sa_addp(v128 vn) { return sa_mk((u64)vn[0] + (u64)vn[1]); }
