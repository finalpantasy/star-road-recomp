/* Star Road Recomp boot driver (task-5): smrpg_boot [--max-seconds N] [--log path] [--img dir] [--entry start|nnmain|addr:HEX]
 *                                                   [--continue] [--no-init] [--trace]
 * Reserves guest memory, loads and relocates the modules, registers the generated chunks, then runs the
 * guest the way rtld does: nn::init::Start(thread handle, rtld end, flag fn, run-init fn, run-fini fn).
 * Stops with a clear log line once the first nn service request is logged. Our own code. */
#include "rt_internal.h"
#include "platform/plat.h"
int svc_thread_registry(RtThread **out, int max);   /* svc.c */

static int g_no_init;
static int g_max_seconds = 60;

/* ---- the three functions rtld hands to nn::init::Start (see runtime-handover.md) ---- */
static void rtld_flag(Ctx *c) { (void)c; }

static void run_array(Ctx *c, int mi, int fini) {
    Module *m = &g_mod[mi];
    uint32_t off = fini ? m->fini_off : m->init_off, sz = fini ? m->fini_sz : m->init_sz;
    uint32_t n = sz / 8;
    const uint64_t *arr = (const uint64_t *)(m->base + off);
    uint64_t lr = c->x[30], pc = c->pc;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = fini ? n - 1 - k : k;
        uint64_t fn = arr[i];
        if (!fn || fn == ~0ULL) continue;
        rt_log("%s %s[%u/%u] %#llx (%s+%#llx)", fini ? "dtor" : "ctor", m->name, i, n, (unsigned long long)fn, m->name,
               (unsigned long long)(fn - m->base));
        for (int r = 0; r < 8; r++) c->x[r] = 0;
        rt_call_guest(c, fn);
    }
    c->x[30] = lr; c->pc = pc;
}

static void rtld_run_init(Ctx *c) {
    static const int order[3] = {1, 2, 0};            /* sdk, subsdk0, main: dependencies first */
    for (int k = 0; k < 3; k++) {
        rt_log("rtld: running init_array of %s (%u entries)", g_mod[order[k]].name, g_mod[order[k]].init_sz / 8);
        run_array(c, order[k], 0);
    }
    rt_log("rtld: all init_arrays done");
}

static void rtld_run_fini(Ctx *c) {
    static const int order[3] = {0, 2, 1};
    rt_log("rtld: running fini_arrays");
    for (int k = 0; k < 3; k++) run_array(c, order[k], 1);
}

static void sentinel_fn(Ctx *c) { rt_fatal(c, "guest control reached the return sentinel through a dispatch", c->pc); }

/* ---- boot thread ---- */
static const char *g_entry = "start";

static DWORD WINAPI boot_thread(LPVOID unused) {
    (void)unused;
    RtThread *t = svc_main_thread();
    Ctx *c = rt_ctx_new(t);
    tl_ctx = c;
    (void)rt_tid();
    uint64_t stack = rt_stack_alloc(16u << 20, 0x1000);
    t->stack_base = stack;
    t->stack_top = stack + (16u << 20) - 0x100;
    c->x[31] = t->stack_top;
    rt_log("main guest thread: handle=%#x sp=%#llx (16 MB; main.npdm says main thread stack 1 MB, priority 44) tlr=%#llx", t->handle,
           (unsigned long long)c->x[31], (unsigned long long)t->tlr);

    if (!strcmp(g_entry, "start")) {
        uint64_t start = ld_find_export("_ZN2nn4init5StartEmmPFvvES2_S2_");
        if (!start) { rt_log("FATAL sdk does not export nn::init::Start"); rt_exit(10, "no entry"); }
        uint64_t rtld_end = rt_stack_alloc(0x4000, 0x1000) + 0x4000;
        rt_log("entry: nn::init::Start at %#llx (sdk+%#llx), as rtld calls it: x0=thread handle, x1=rtld end, x2=flag fn, x3=run init_arrays, x4=run fini_arrays",
               (unsigned long long)start, (unsigned long long)(start - GUEST_MOD_SDK));
        c->x[0] = t->handle;
        c->x[1] = rtld_end;
        c->x[2] = hle_slot("<rtld:set-started-flag>", SLOT_INTERNAL, rtld_flag, 0)->addr;
        c->x[3] = hle_slot("<rtld:run-init-arrays>", SLOT_INTERNAL, rtld_run_init, 0)->addr;
        c->x[4] = hle_slot("<rtld:run-fini-arrays>", SLOT_INTERNAL, rtld_run_fini, 0)->addr;
        rt_call_guest(c, start);
    } else {
        if (!g_no_init) rtld_run_init(c);
        uint64_t target = 0;
        if (!strcmp(g_entry, "nnmain")) target = ld_find_export("nnMain");
        else if (!strncmp(g_entry, "addr:", 5)) target = strtoull(g_entry + 5, 0, 16);
        if (!target) { rt_log("FATAL no entry address for --entry %s", g_entry); rt_exit(10, "no entry"); }
        rt_log("entry: %s at %#llx", g_entry, (unsigned long long)target);
        for (int r = 0; r < 8; r++) c->x[r] = 0;
        rt_call_guest(c, target);
    }
    rt_log("main guest thread returned from its entry (x0=%#llx)", (unsigned long long)c->x[0]);
    return 0;
}

/* SMRPG_PROF=<ms>: sampling profiler (debug aid). Every <ms> each running guest thread is suspended and its host RIP recorded (image offset, resolve with
 * llvm-symbolizer on smrpg.exe) together with its last guest call-out lr. Group 0 is the guest main thread, 1 every other thread. A RIP outside the image
 * (ntdll waits and the like) is counted as "outside". The table is dumped by prof_dump at exit and at the watchdog. */
#define PROF_CAP 8192
typedef struct { uint64_t key; uint32_t n; } ProfEnt;
static ProfEnt g_prof_rip[2][PROF_CAP], g_prof_lr[2][PROF_CAP];
static uint64_t g_prof_out[2], g_prof_samples;
static volatile LONG g_prof_on;
static HANDLE g_boot_host;   /* the guest main thread has no RtThread host handle; this is its host thread */
static void prof_add(ProfEnt *t, uint64_t key) {
    uint32_t h = (uint32_t)((key >> 2) * 2654435761u) & (PROF_CAP - 1);
    for (int i = 0; i < PROF_CAP; i++, h = (h + 1) & (PROF_CAP - 1)) {
        if (t[h].key == key) { t[h].n++; return; }
        if (!t[h].key) { t[h].key = key; t[h].n = 1; return; }
    }
}
static DWORD WINAPI prof_main(LPVOID p) {
    DWORD ms = (DWORD)(uintptr_t)p;
    uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        Sleep(ms);
        RtThread *th[64];
        int n = svc_thread_registry(th, 64);
        for (int i = 0; i < n; i++) {
            int g = strcmp(th[i]->name, "main") == 0 ? 0 : 1;
            HANDLE hh = g == 0 && !th[i]->host ? g_boot_host : th[i]->host;
            if (!hh || th[i]->exited || hh == GetCurrentThread() || (g && g_prof_samples % 10)) continue;   /* the other threads are sampled every 10th round */
            if (SuspendThread(hh) == (DWORD)-1) continue;
            CONTEXT cx; memset(&cx, 0, sizeof cx); cx.ContextFlags = CONTEXT_CONTROL;
            uint64_t rip = GetThreadContext(hh, &cx) ? cx.Rip : 0, lr = th[i]->ctx ? th[i]->ctx->x[30] : 0;
            ResumeThread(hh);
            if (!rip) continue;
            if (rip >= base && rip < base + 0x20000000ull) { prof_add(g_prof_rip[g], rip - base); prof_add(g_prof_lr[g], lr); }
            else g_prof_out[g]++;
        }
        g_prof_samples++;
        g_prof_on = 1;
    }
}
static int prof_cmp(const void *a, const void *b) { return (int)((const ProfEnt *)b)->n - (int)((const ProfEnt *)a)->n; }
void prof_dump(void) {
    static volatile LONG once;
    if (!g_prof_on || InterlockedExchange(&once, 1)) return;
    rt_log("PROF: %llu sample rounds", (unsigned long long)g_prof_samples);
    static const char *gn[2] = {"main thread", "other threads"};
    for (int g = 0; g < 2; g++) {
        for (int k = 0; k < 2; k++) {
            ProfEnt *t = k ? g_prof_lr[g] : g_prof_rip[g];
            qsort(t, PROF_CAP, sizeof(ProfEnt), prof_cmp);
            uint64_t tot = g_prof_out[g];
            for (int i = 0; i < PROF_CAP && t[i].n; i++) tot += t[i].n;
            rt_log("PROF %s, %s: %llu samples in image, %llu outside (waiting)", gn[g], k ? "last guest call-out lr" : "host rip (image offset)",
                   (unsigned long long)(tot - g_prof_out[g]), (unsigned long long)g_prof_out[g]);
            for (int i = 0; i < 40 && t[i].n; i++) rt_log("PROF   %s %#llx x%u", k ? "lr" : "rip", (unsigned long long)t[i].key, t[i].n);
        }
    }
}

static DWORD WINAPI watchdog_main(LPVOID p) {
    DWORD ms = (DWORD)(uintptr_t)p;
    Sleep(ms);
    rt_log("WATCHDOG: no result after %lu s, stopping", (unsigned long)(ms / 1000));
    prof_dump();
    /* where is every guest thread? host RIP minus the image base resolves with llvm-symbolizer on the unstripped smrpg.exe */
    {
        RtThread *th[64];
        int n = svc_thread_registry(th, 64);
        uint64_t rip[64] = {0}, base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
        for (int i = 0; i < n; i++) {
            if (!th[i]->host || th[i]->exited || th[i]->host == GetCurrentThread()) continue;
            if (SuspendThread(th[i]->host) == (DWORD)-1) continue;
            CONTEXT cx; memset(&cx, 0, sizeof cx); cx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th[i]->host, &cx)) rip[i] = cx.Rip;
            ResumeThread(th[i]->host);
        }
        for (int i = 0; i < n; i++)
            rt_log("WATCHDOG thread tid=%u handle=%#x '%s' host rip=%#llx (image+%#llx) guest lr=%#llx", th[i]->tid, th[i]->handle, th[i]->name,
                   (unsigned long long)rip[i], (unsigned long long)(rip[i] ? rip[i] - base : 0), (unsigned long long)(th[i]->ctx ? th[i]->ctx->x[30] : 0));
        /* guest frame chain from the saved x29 (flushed at every call out of lifted code); an unreadable link ends the walk */
        for (int i = 0; i < n; i++) {
            if (!th[i]->ctx) continue;
            char bt[400]; int o = 0;
            uint64_t fp = th[i]->ctx->x[29];
            for (int d = 0; d < 14 && fp >= 0x1000 && !(fp & 7) && o < (int)sizeof bt - 24; d++) {
                uint64_t lr = 0, nfp = 0;
                if (IsBadReadPtr((void *)(uintptr_t)fp, 16)) break;
                nfp = ((uint64_t *)fp)[0]; lr = ((uint64_t *)fp)[1];
                o += snprintf(bt + o, sizeof bt - o, " %#llx", (unsigned long long)lr);
                if (nfp <= fp) break;
                fp = nfp;
            }
            rt_log("WATCHDOG tid=%u guest frames:%s", th[i]->tid, bt);
        }
    }
    /* SMRPG_WDUMP=addr:len (hex) prints guest memory rows when the watchdog fires (debugging stuck boots) */
    if (getenv("SMRPG_WDUMP")) {
        unsigned long long a = 0, l = 0;
        if (sscanf(getenv("SMRPG_WDUMP"), "%llx:%llx", &a, &l) == 2 && l <= 0x2000 && !IsBadReadPtr((void *)(uintptr_t)a, (size_t)l))
            for (unsigned long long o = 0; o < l; o += 32) {
                const uint32_t *w = (const uint32_t *)(uintptr_t)(a + o);
                rt_log("WDUMP %#llx: %08x %08x %08x %08x %08x %08x %08x %08x", a + o, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
    }
    for (int i = 0; i < hle_nslots(); i++) {                 /* nn::os calls the guest made through imported slots: what did the stuck threads (not) do */
        HleSlot *s = hle_slot_n(i);
        if (s->count && getenv("SMRPG_SLOTS")) rt_log("WATCHDOG slot %lld x %s", (long long)s->count, s->name);
    }
    rt_ring_dump();
    rt_exit(4, "watchdog timeout");
    return 0;
}

void gfxcap_init(void);   /* gfxcap.c */
/* task-22: --window shows the window, --audio opens a real audio device, both off by default. The options parse in every build; they act
 * only when the platform layer (SDL3) is linked. */
static PlatOptions g_plat;

int main(int argc, char **argv) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    rt_reserve_early();
    const char *logp = "smrpg_boot.log";
    const char *img = getenv("SMRPG_IMG");
    if (!img || !*img) img = "A:\\AI\\Recomps\\smrpg-work\\build\\img";
    const char *tr = getenv("SMRPG_TRACE");
    g_trace = tr && *tr && strcmp(tr, "0") != 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--max-seconds") && i + 1 < argc) g_max_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
        else if (!strcmp(argv[i], "--img") && i + 1 < argc) img = argv[++i];
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc) g_entry = argv[++i];
        else if (!strcmp(argv[i], "--continue")) g_continue = 1;
        else if (!strcmp(argv[i], "--no-init")) g_no_init = 1;
        else if (!strcmp(argv[i], "--trace")) g_trace = 1;
        else if (!strcmp(argv[i], "--window")) g_plat.show_window = 1;
        else if (!strcmp(argv[i], "--audio")) g_plat.audio = 1;
        else if (!strcmp(argv[i], "--fullscreen")) g_plat.fullscreen = 1;
        else if (!strcmp(argv[i], "--input-config") && i + 1 < argc) { g_plat.input_config = argv[++i]; _putenv_s("SMRPG_INPUT_CONFIG", argv[i]); }
        else { fprintf(stderr, "usage: smrpg_boot [--max-seconds N] [--log path] [--img dir] [--entry start|nnmain|addr:HEX] [--continue] [--no-init] [--trace] [--window [--fullscreen]] [--audio] [--input-config ini]\n"); return 2; }
    }
    rt_log_open(logp);
    rt_log("smrpg_boot: img=%s entry=%s max-seconds=%d trace=%d", img, g_entry, g_max_seconds, g_trace);
    rt_install_veh();
    rt_fn_tables_init();
    rt_reg(RT_SENTINEL, sentinel_fn);
    svc_init();
#ifdef SMRPG_PLATFORM
    if (plat_init(&g_plat)) rt_exit(10, "platform layer failed to start");
    plat_register_nvn();      /* the SDL window becomes NVN's present surface (before the first NVN device exists) */
#else
    if (g_plat.show_window || g_plat.audio) rt_log("this build has no platform layer (SDL3 not found): --window and --audio are ignored");
#endif
    gfxcap_init();
    rt_reserve_regions();
    if (ld_load_all(img)) rt_exit(10, "load failed");
    if (g_reg_nchunks == 0) rt_log("no game library is linked (g_reg_nchunks = 0): every guest address will miss and need the interpreter");
    const char *nolift = getenv("SMRPG_NO_LIFTED");   /* differential test: run everything in the IR interpreter */
    if (nolift && *nolift == '1') rt_log("SMRPG_NO_LIFTED=1: lifted functions are not registered, the interpreter runs all guest code");
    else for (uint32_t i = 0; i < g_reg_nchunks; i++) g_reg_chunks[i]();
    rt_log("registered %u generated chunks", g_reg_nchunks);

    if (getenv("SMRPG_PROF")) CreateThread(NULL, 0, prof_main, (LPVOID)(uintptr_t)(DWORD)atoi(getenv("SMRPG_PROF")), 0, NULL);
    CreateThread(NULL, 0, watchdog_main, (LPVOID)(uintptr_t)((DWORD)g_max_seconds * 1000), 0, NULL);
    HANDLE bt = CreateThread(NULL, 64u << 20, boot_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    g_boot_host = bt;
    if (!bt) rt_exit(10, "cannot create boot thread");
#ifdef SMRPG_PLATFORM
    plat_run(bt);             /* main thread: SDL events, pad state into the hid block every 5 ms, until the boot thread ends */
#else
    WaitForSingleObject(bt, INFINITE);
#endif
    rt_exit(g_first_service ? 0 : 1, g_first_service ? "boot thread ended after the first service request" : "boot thread ended before any nn service request");
    return 0;
}
