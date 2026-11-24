/* Task-4 spike bench runtime. Our own code.
 *
 * Maps the relocated guest image at its own address (identity mapping), runs lifted leaf
 * functions from a fixed start state, hashes the end state (for cross-variant checks) and
 * times them. Guest faults are caught by a vectored handler and reported, not fatal.
 * Also times indirect-call dispatch lookups over the full function address list.
 *
 * usage: bench.exe <guest.elf> <mode: screen|time> <list.txt> <out.jsonl> [<all_addrs.txt>]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spike_rt.h"

extern const u64 g_fn_addr[];
extern void (*const g_fn_ptr[])(Ctx *restrict);
extern const u32 g_fn_count;

u8 *g_membase = 0;
volatile int g_rt_poll = 0;
static void *g_jb[5];
static volatile int g_in_guest = 0;
static volatile LONGLONG g_start_tick = 0;
static LARGE_INTEGER g_freq;

#define GUEST_BASE 0x7100000000ULL
#define SCRATCH 0x7200000000ULL
#define SCRATCH_SIZE (8u << 16)
#define STACK 0x7300000000ULL
#define STACK_SIZE (1u << 20)

void rt_missing(Ctx *c, u64 a) { (void)c; (void)a; }
void rt_icall(Ctx *c, u64 t) { (void)c; (void)t; }
void rt_dispatch(Ctx *c) { (void)c; }
void rt_svc(Ctx *c, u32 i) { (void)c; (void)i; }
void rt_brk(Ctx *c, u32 i) { (void)c; (void)i; }
u64 rt_cntvct(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (u64)t.QuadPart; }
static __attribute__((noinline, noreturn)) void recover(void) { __builtin_longjmp(g_jb, 1); }
void rt_safepoint(Ctx *c) { (void)c; if (g_rt_poll) __builtin_longjmp(g_jb, 1); }

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!g_in_guest) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) {
        ep->ContextRecord->Rsp = (ep->ContextRecord->Rsp & ~15ULL) - 8;
        ep->ContextRecord->Rip = (DWORD64)(uintptr_t)recover;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(20);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (g_in_guest && (t.QuadPart - g_start_tick) * 1000 / g_freq.QuadPart > 200) g_rt_poll = 1;
    }
}

static int map_guest(const char *elf) {
    FILE *f = fopen(elf, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) return 0;
    fclose(f);
    u64 phoff = *(u64 *)(buf + 0x20);
    u16 phent = *(u16 *)(buf + 0x36), phnum = *(u16 *)(buf + 0x38);
    u64 lo = ~0ULL, hi = 0;
    for (int k = 0; k < phnum; k++) {
        u8 *ph = buf + phoff + k * phent;
        if (*(u32 *)ph != 1) continue;
        u64 va = *(u64 *)(ph + 16), msz = *(u64 *)(ph + 40);
        if (va < lo) lo = va;
        if (va + msz > hi) hi = va + msz;
    }
    void *m = VirtualAlloc((void *)(uintptr_t)lo, (hi - lo + 0xFFFF) & ~0xFFFFULL, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (m != (void *)(uintptr_t)lo) { fprintf(stderr, "identity map failed at %llx\n", (unsigned long long)lo); return 0; }
    for (int k = 0; k < phnum; k++) {
        u8 *ph = buf + phoff + k * phent;
        if (*(u32 *)ph != 1) continue;
        memcpy((void *)(uintptr_t)*(u64 *)(ph + 16), buf + *(u64 *)(ph + 8), *(u64 *)(ph + 32));
    }
    free(buf);
    if (VirtualAlloc((void *)SCRATCH, SCRATCH_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)SCRATCH) return 0;
    if (VirtualAlloc((void *)STACK, STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)STACK) return 0;
    return 1;
}

static void set_args(Ctx *c) {
    for (int k = 0; k < 8; k++) {
        c->x[k] = SCRATCH + (u64)k * 0x10000 + 0x100;
        c->v[k].l = d2u(1.25 + k);
        c->v[k].h = 0;
    }
    c->sp = STACK + STACK_SIZE - 0x1000;
}

static void reset(Ctx *c) {
    memset(c, 0, sizeof *c);
    memset((void *)SCRATCH, 0, SCRATCH_SIZE);
    set_args(c);
    c->x[29] = c->sp;
    c->excl_addr = ~0ULL;
}

static u64 fnv(u64 h, const void *p, size_t n) {
    const u8 *b = p;
    for (size_t k = 0; k < n; k++) h = (h ^ b[k]) * 0x100000001b3ULL;
    return h;
}

static u64 state_hash(Ctx *c) {
    /* AAPCS64-visible end state only: results, callee-saved registers, sp, memory. Caller-saved
       temporaries and flags are dead after a return, and the locals model does not write them back. */
    u64 h = 0xcbf29ce484222325ULL;
    h = fnv(h, &c->x[0], 16);
    h = fnv(h, &c->x[19], 11 * 8);
    h = fnv(h, &c->sp, 8);
    for (int k = 0; k < 8; k++) h = fnv(h, &c->v[k], 16);
    for (int k = 8; k < 16; k++) h = fnv(h, &c->v[k].l, 8);
    return fnv(h, (void *)SCRATCH, SCRATCH_SIZE);
}

typedef void (*FnPtr)(Ctx *restrict);

/* returns 0 ok, 1 fault or timeout */
static int guarded_run(FnPtr f, Ctx *c, int iters, double *ns) {
    static volatile int rc;
    rc = 0;
    if (__builtin_setjmp(g_jb)) {
        g_in_guest = 0;
        g_rt_poll = 0;
        return 1;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_start_tick = t0.QuadPart;
    g_in_guest = 1;
    for (int k = 0; k < iters; k++) {
        set_args(c);
        f(c);
    }
    g_in_guest = 0;
    QueryPerformanceCounter(&t1);
    if (ns) *ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / g_freq.QuadPart / iters;
    return rc;
}

static void noop_fn(Ctx *restrict c) { __asm__ volatile("" ::"r"(c) : "memory"); }

static int find(u64 a) {
    for (u32 k = 0; k < g_fn_count; k++)
        if (g_fn_addr[k] == a) return (int)k;
    return -1;
}

/* dispatch lookup cost: sorted array binary search versus a dense table indexed by (addr-base)/4 */
static void bench_dispatch(const char *path, FILE *out) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    u64 *addrs = malloc(sizeof(u64) * 200000);
    u32 n = 0;
    unsigned long long a;
    while (n < 200000 && fscanf(f, "%llx", &a) == 1) addrs[n++] = a;
    fclose(f);
    u64 text_slots = 0x2400000 / 4;
    void **dense = VirtualAlloc(0, text_slots * sizeof(void *), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (u32 k = 0; k < n; k++) dense[(addrs[k] - GUEST_BASE) / 4] = (void *)(uintptr_t)(k + 1);
    const int Q = 4000000;
    u64 *q = malloc(sizeof(u64) * Q);
    u64 seed = 12345;
    for (int pass = 0; pass < 2; pass++) {
        u32 span = pass == 0 ? n : 512; /* all functions (cold) versus a 512-target hot set */
        for (int k = 0; k < Q; k++) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; q[k] = addrs[(seed >> 33) % span]; }
        LARGE_INTEGER t0, t1;
        u64 sink = 0;
        QueryPerformanceCounter(&t0);
        for (int k = 0; k < Q; k++) {
            u64 x = q[k];
            u32 lo = 0, hi = n;
            while (lo < hi) { u32 m = (lo + hi) >> 1; if (addrs[m] < x) lo = m + 1; else hi = m; }
            sink += lo;
        }
        QueryPerformanceCounter(&t1);
        double bs = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / g_freq.QuadPart / Q;
        QueryPerformanceCounter(&t0);
        for (int k = 0; k < Q; k++) sink += (uintptr_t)dense[(q[k] - GUEST_BASE) >> 2];
        QueryPerformanceCounter(&t1);
        double dn = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / g_freq.QuadPart / Q;
        fprintf(out, "{\"dispatch\":\"%s\",\"targets\":%u,\"bsearch_ns\":%.2f,\"dense_ns\":%.2f,\"sink\":%llu}\n",
                pass == 0 ? "cold_all" : "hot_512", span, bs, dn, (unsigned long long)(sink & 1));
    }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
    QueryPerformanceFrequency(&g_freq);
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << 2);
    if (!map_guest(argv[1])) return 3;
    AddVectoredExceptionHandler(1, veh);
    CreateThread(0, 0, watchdog, 0, 0, 0);
    int timing = strcmp(argv[2], "time") == 0;
    FILE *lf = fopen(argv[3], "r"), *out = fopen(argv[4], "w");
    if (!lf || !out) return 4;
    static Ctx c;
    double base_ns = 1e9, ns;
    for (int r = 0; r < 5; r++) { reset(&c); guarded_run(noop_fn, &c, 20000, &ns); if (ns < base_ns) base_ns = ns; }
    fprintf(out, "{\"loop_overhead_ns\":%.3f}\n", base_ns);
    unsigned long long a;
    while (fscanf(lf, "%llx", &a) == 1) {
        int k = find(a);
        if (k < 0) continue;
        FnPtr f = g_fn_ptr[k];
        reset(&c);
        int bad = guarded_run(f, &c, 1, 0);
        u64 h = bad ? 0 : state_hash(&c);
        if (!bad) { reset(&c); bad = guarded_run(f, &c, 3, 0); }
        double best = 1e18;
        if (!bad && timing) {
            for (int r = 0; r < 7 && !bad; r++) {
                reset(&c);
                bad = guarded_run(f, &c, 5000, &ns);
                if (ns < best) best = ns;
            }
        }
        fprintf(out, "{\"addr\":\"%llx\",\"ok\":%d,\"hash\":\"%016llx\",\"ns\":%.3f}\n", a, !bad,
                (unsigned long long)h, timing && !bad ? best - base_ns : -1.0);
        fflush(out);
    }
    if (argc > 5) bench_dispatch(argv[5], out);
    fclose(out);
    return 0;
}
