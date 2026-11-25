/* Freestanding helpers for the task-14 AArch64 test modules (built with clang --target=aarch64-linux-gnu, no libc).
 * Our own code. Output goes through svcOutputDebugString, which the runtime logs as "guest debug output: ...". */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long u64;
typedef unsigned int u32;

static inline u64 svc_out(const char *s, u64 n) {
    register u64 x0 __asm__("x0") = (u64)s;
    register u64 x1 __asm__("x1") = n;
    __asm__ volatile("svc #0x27" : "+r"(x0) : "r"(x1) : "memory");
    return x0;
}
static inline u64 g_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static inline void out(const char *s) { svc_out(s, g_strlen(s)); }

/* "<prefix><decimal>" */
static inline void out_num(const char *prefix, long v) {
    char b[96];
    u64 n = 0;
    while (prefix[n] && n < 60) { b[n] = prefix[n]; n++; }
    char t[24];
    int k = 0;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[n++] = '-';
    while (k) b[n++] = t[--k];
    svc_out(b, n);
}

static inline u64 svc6(u32 id, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 *r1) {
    register u64 x0 __asm__("x0") = a0;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    register u64 x4 __asm__("x4") = a4;
    register u64 x5 __asm__("x5") = a5;
    switch (id) {
    case 0x08: __asm__ volatile("svc #0x08" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory"); break;
    case 0x09: __asm__ volatile("svc #0x09" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory"); break;
    case 0x0B: __asm__ volatile("svc #0x0B" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory"); break;
    case 0x32: __asm__ volatile("svc #0x32" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory"); break;
    case 0x33: __asm__ volatile("svc #0x33" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory"); break;
    default: __builtin_trap();
    }
    if (r1) *r1 = x1;
    return x0;
}
#ifdef __cplusplus
}
#endif
