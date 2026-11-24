/* Star Road Recomp runtime core (task-5): log, dispatch tables, indirect calls, fatal paths, crash handler.
 * Our own code. */
#include "rt_internal.h"

uint32_t *g_fn[RT_NMOD];
int g_trace;
int g_continue;
_Thread_local Ctx *tl_ctx;

/* ------------------------------------------------------------------ log */
static FILE *g_logf;
static CRITICAL_SECTION g_logcs;
static LARGE_INTEGER g_qpf, g_q0;
static volatile LONG g_tidnext = 1;
static _Thread_local uint32_t tl_tid;

double rt_now(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_q0.QuadPart) / (double)g_qpf.QuadPart;
}

uint64_t rt_cntvct(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t f = (uint64_t)g_qpf.QuadPart, v = (uint64_t)q.QuadPart;
    return (v / f) * RT_CNTFRQ + (v % f) * RT_CNTFRQ / f;
}

uint32_t rt_tid(void) {
    if (!tl_tid) tl_tid = (uint32_t)InterlockedIncrement(&g_tidnext) - 1;
    return tl_tid;
}

void rt_log_open(const char *path) {
    static int once;
    if (!once) {
        once = 1;
        InitializeCriticalSection(&g_logcs);
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_q0);
    }
    if (path && *path && !g_logf) g_logf = fopen(path, "w");
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[%8.3f T%u] ", rt_now(), rt_tid());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    va_end(ap);
    size_t l = strlen(buf);
    buf[l] = '\n';
    buf[l + 1] = 0;
    EnterCriticalSection(&g_logcs);
    fputs(buf, stdout);
    fflush(stdout);
    if (g_logf) {
        fputs(buf, g_logf);
        fflush(g_logf);
    }
    LeaveCriticalSection(&g_logcs);
}

void rt_log_flush(void) {
    EnterCriticalSection(&g_logcs);
    fflush(stdout);
    if (g_logf) fflush(g_logf);
    LeaveCriticalSection(&g_logcs);
}

const char *rt_safe_str(uint64_t addr, char *out, size_t n) {
    SIZE_T got = 0;
    out[0] = 0;
    if (addr < 0x10000) return out;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n - 1, &got) && got == 0) return out;
    out[got] = 0;
    for (size_t i = 0; i < got; i++) {
        if (!out[i]) break;
        if ((unsigned char)out[i] < 32 || (unsigned char)out[i] > 126) { out[i] = 0; break; }
    }
    return out;
}

/* ------------------------------------------------------------------ ring buffer */
typedef struct { const char *kind, *name; uint64_t a0, a1, a2; uint32_t tid; double t; } RingEnt;
static RingEnt g_ring[32];
static volatile LONG g_ring_i;

void rt_ring_push(const char *kind, const char *name, uint64_t a0, uint64_t a1, uint64_t a2) {
    LONG i = InterlockedIncrement(&g_ring_i) - 1;
    RingEnt *e = &g_ring[i & 31];
    e->kind = kind; e->name = name; e->a0 = a0; e->a1 = a1; e->a2 = a2; e->tid = rt_tid(); e->t = rt_now();
}

void rt_ring_dump(void) {
    LONG end = g_ring_i, start = end > 32 ? end - 32 : 0;
    rt_log("last %d HLE/svc calls (oldest first):", (int)(end - start));
    for (LONG i = start; i < end; i++) {
        RingEnt *e = &g_ring[i & 31];
        if (!e->kind) continue;
        rt_log("  [%8.3f T%u] %s %s x0=%#llx x1=%#llx x2=%#llx", e->t, e->tid, e->kind, e->name,
               (unsigned long long)e->a0, (unsigned long long)e->a1, (unsigned long long)e->a2);
    }
}

/* ------------------------------------------------------------------ dispatch tables */
static char *g_fn_arena;
static uint32_t g_fn_committed[RT_NMOD];

void rt_fn_tables_init(void) {
    uint64_t per = (GUEST_MOD_SPAN >> 2) * sizeof(uint32_t);
    g_fn_arena = VirtualAlloc(NULL, per * RT_NMOD, MEM_RESERVE, PAGE_READWRITE);
    if (!g_fn_arena) { rt_log("FATAL cannot reserve dispatch tables"); rt_exit(10, "fn table reserve"); }
    for (int m = 0; m < RT_NMOD; m++) g_fn[m] = (uint32_t *)(g_fn_arena + per * (uint64_t)m);
}

static void fn_commit(int m, uint64_t bytes) {
    bytes = (bytes + 0xFFFF) & ~0xFFFFULL;
    if (bytes > GUEST_MOD_SPAN) bytes = GUEST_MOD_SPAN;
    if (bytes <= g_fn_committed[m]) return;
    if (!VirtualAlloc((char *)g_fn[m], bytes, MEM_COMMIT, PAGE_READWRITE)) {
        rt_log("FATAL cannot commit dispatch table %d (%llu bytes)", m, (unsigned long long)bytes);
        rt_exit(10, "fn table commit");
    }
    g_fn_committed[m] = (uint32_t)bytes;
}

void rt_fn_commit_module(int m, uint64_t image_bytes) { fn_commit(m, image_bytes); }

void rt_reg(uint64_t a, GuestFn f) {
    extern char __ImageBase;
    uint64_t m = (a >> 28) & 0xF;
    if ((a >> 32) != 0x71 || m >= RT_NMOD) {
        rt_log("FATAL rt_reg of address %#llx outside the module slots", (unsigned long long)a);
        rt_exit(10, "rt_reg");
    }
    uint64_t idx = (a & (GUEST_MOD_SPAN - 1)) >> 2;
    if ((idx + 1) * 4 > g_fn_committed[m]) fn_commit((int)m, (idx + 1) * 4 + 0x10000);
    g_fn[m][idx] = (uint32_t)((char *)f - &__ImageBase);
}

/* ------------------------------------------------------------------ miss profile */
typedef struct { uint64_t addr, caller; uint64_t count; } MissEnt;
#define MISS_CAP 65536
static MissEnt g_miss[MISS_CAP];
static uint32_t g_nmiss;
static CRITICAL_SECTION g_misscs;
static volatile LONG g_miss_init;

static void miss_note(uint64_t addr, uint64_t caller) {
    if (InterlockedCompareExchange(&g_miss_init, 1, 0) == 0) { InitializeCriticalSection(&g_misscs); g_miss_init = 2; }
    while (g_miss_init != 2) Sleep(0);
    EnterCriticalSection(&g_misscs);
    uint32_t h = (uint32_t)((addr >> 2) * 2654435761u) & (MISS_CAP - 1);
    while (g_miss[h].addr && g_miss[h].addr != addr) h = (h + 1) & (MISS_CAP - 1);
    if (!g_miss[h].addr) {
        g_miss[h].addr = addr; g_miss[h].caller = caller; g_nmiss++;
        const Module *m = ld_module_of(addr);
        if (g_trace || g_nmiss <= 20)
            rt_log("MISS new indirect target %#llx (%s+%#llx) from lr=%#llx -> interpreter", (unsigned long long)addr,
                   m ? m->name : "?", (unsigned long long)(m ? addr - m->base : 0), (unsigned long long)caller);
    }
    g_miss[h].count++;
    LeaveCriticalSection(&g_misscs);
}

static void miss_dump(void) {
    if (!g_nmiss) { rt_log("interpreter misses: none"); return; }
    rt_log("interpreter misses: %u distinct targets (addr, count, first caller)", g_nmiss);
    int shown = 0;
    for (uint32_t i = 0; i < MISS_CAP && shown < 40; i++)
        if (g_miss[i].addr) { rt_log("  miss %#llx x%llu from %#llx", (unsigned long long)g_miss[i].addr, (unsigned long long)g_miss[i].count, (unsigned long long)g_miss[i].caller); shown++; }
    const char *p = getenv("SMRPG_PROFILE");
    if (p && *p) {
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "addr\tcount\tcaller\n");
            for (uint32_t i = 0; i < MISS_CAP; i++)
                if (g_miss[i].addr) fprintf(f, "%#llx\t%llu\t%#llx\n", (unsigned long long)g_miss[i].addr, (unsigned long long)g_miss[i].count, (unsigned long long)g_miss[i].caller);
            fclose(f);
        }
    }
}

/* ------------------------------------------------------------------ calls */
void rt_icall(Ctx *c, uint64_t t) {
    GuestFn f = rt_lookup(t);
    c->pc = t;                        /* HLE thunks identify their slot from c->pc */
    if (f) { f(c); return; }
    if ((t >> 32) != 0x71 || ((t >> 28) & 0xF) >= RT_NMOD) {
        rt_fatal(c, t ? "indirect call to a non-code address" : "indirect call to null", t);
        return;
    }
    miss_note(t, c->x[30]);
    interp_run(c, t);
}

void rt_dispatch(Ctx *c) { rt_icall(c, c->pc); }

void rt_poll(Ctx *c) { (void)c; }

void rt_call_guest(Ctx *c, uint64_t addr) {
    c->x[30] = RT_SENTINEL;
    c->exc = 0;
    rt_icall(c, addr);
    if (c->exc) {
        char nm[160];
        const char *name = "?";
        if (c->exc_type > 0x10000) {
            uint64_t np = 0;
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(c->exc_type + 8), &np, 8, &got) && got == 8)
                name = rt_safe_str(np, nm, sizeof nm);
        }
        rt_log("UNCAUGHT C++ EXCEPTION at guest call boundary: object=%p type_info=%#llx name=%s", c->exc,
               (unsigned long long)c->exc_type, name);
        c->exc = 0;
    }
}

/* ------------------------------------------------------------------ fatal */
static volatile LONG g_dying;

static void dump_ctx(const Ctx *c) {
    rt_log("guest ctx: pc=%#llx sp=%#llx lr=%#llx fp=%#llx", (unsigned long long)c->pc, (unsigned long long)c->x[31],
           (unsigned long long)c->x[30], (unsigned long long)c->x[29]);
    rt_log("  x0-x7: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx", (unsigned long long)c->x[0], (unsigned long long)c->x[1],
           (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4], (unsigned long long)c->x[5],
           (unsigned long long)c->x[6], (unsigned long long)c->x[7]);
    rt_log("  x19-x28: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx", (unsigned long long)c->x[19], (unsigned long long)c->x[20],
           (unsigned long long)c->x[21], (unsigned long long)c->x[22], (unsigned long long)c->x[23], (unsigned long long)c->x[24],
           (unsigned long long)c->x[25], (unsigned long long)c->x[26], (unsigned long long)c->x[27], (unsigned long long)c->x[28]);
    const Module *m = ld_module_of(c->x[30]);
    if (m) rt_log("  lr is %s+%#llx", m->name, (unsigned long long)(c->x[30] - m->base));
    m = ld_module_of(c->pc);
    if (m) rt_log("  pc is %s+%#llx", m->name, (unsigned long long)(c->pc - m->base));
}

void rt_fatal(Ctx *c, const char *what, uint64_t arg) {
    if (InterlockedExchange(&g_dying, 1)) { Sleep(INFINITE); }
    rt_log("FATAL: %s (%#llx)", what, (unsigned long long)arg);
    if (c) dump_ctx(c);
    rt_ring_dump();
    rt_exit(3, what);
}

void rt_brk(Ctx *c, uint32_t imm) { rt_fatal(c, "brk instruction executed", imm); }

static LONG WINAPI rt_veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && g_fn_arena) {
        uint64_t fa = (uint64_t)ep->ExceptionRecord->ExceptionInformation[1];
        uint64_t per = (GUEST_MOD_SPAN >> 2) * sizeof(uint32_t);
        if (fa >= (uint64_t)g_fn_arena && fa < (uint64_t)g_fn_arena + per * RT_NMOD) {
            uint64_t pg = fa & ~0xFFFFULL;
            if (VirtualAlloc((void *)pg, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedExchange(&g_dying, 1)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    rt_log("HOST CRASH: code=%#lx host rip=%p fault addr=%#llx (%s)", (unsigned long)code, er->ExceptionAddress,
           (unsigned long long)(er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0),
           er->NumberParameters > 1 ? (er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "exec") : "n/a");
    rt_log("  host rsp=%#llx rbp=%#llx", (unsigned long long)ep->ContextRecord->Rsp, (unsigned long long)ep->ContextRecord->Rbp);
    if (tl_ctx) dump_ctx(tl_ctx);
    rt_ring_dump();
    rt_exit(5, "host crash");
}

void rt_install_veh(void) { AddVectoredExceptionHandler(1, rt_veh); }

/* ------------------------------------------------------------------ exit paths */
static volatile LONG g_summarized;

void rt_summary(void) {
    if (InterlockedExchange(&g_summarized, 1)) return;
    rt_log("---- svc usage (number, name, count) ----");
    for (int i = 0; i < 0x100; i++)
        if (g_svc_count[i]) rt_log("  svc 0x%02x %-28s x%lld", i, svc_name((uint32_t)i), (long long)g_svc_count[i]);
    int nh = hle_nslots(), used = 0;
    rt_log("---- HLE and stub symbols called (kind, symbol, count) ----");
    for (int i = 0; i < nh; i++) {
        HleSlot *s = hle_slot_n(i);
        if (s->count) {
            used++;
            rt_log("  %-5s %-60.60s x%lld%s", s->kind == SLOT_HLE ? "HLE" : s->kind == SLOT_STUB ? "STUB" : s->kind == SLOT_WRAP ? "WRAP" : "INT",
                   s->name, (long long)s->count, s->nn_service ? "  [nn service]" : "");
        }
    }
    rt_log("  %d symbols called of %d slots", used, nh);
    miss_dump();
}

void rt_exit(int code, const char *why) {
    rt_log("EXIT code=%d (%s)", code, why ? why : "");
    rt_summary();
    rt_log_flush();
    ExitProcess((UINT)code);
    for (;;) Sleep(1000);
}

void rt_boot_reached(const char *what, const char *detail) {
    rt_log("========================================================================");
    rt_log("BOOT REACHED: %s %s", what, detail ? detail : "");
    rt_log("========================================================================");
    if (!g_continue) rt_exit(0, "first nn service request reached");
}
