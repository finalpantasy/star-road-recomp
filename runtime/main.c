/* Star Road Recomp boot driver (task-5): smrpg_boot [--max-seconds N] [--log path] [--img dir] [--entry start|nnmain|addr:HEX]
 *                                                   [--continue] [--no-init] [--trace]
 * Reserves guest memory, loads and relocates the modules, registers the generated chunks, then runs the
 * guest the way rtld does: nn::init::Start(thread handle, rtld end, flag fn, run-init fn, run-fini fn).
 * Stops with a clear log line once the first nn service request is logged. Our own code. */
#include "rt_internal.h"

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

static DWORD WINAPI watchdog_main(LPVOID p) {
    DWORD ms = (DWORD)(uintptr_t)p;
    Sleep(ms);
    rt_log("WATCHDOG: no result after %lu s, stopping", (unsigned long)(ms / 1000));
    rt_ring_dump();
    rt_exit(4, "watchdog timeout");
    return 0;
}

int main(int argc, char **argv) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
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
        else { fprintf(stderr, "usage: smrpg_boot [--max-seconds N] [--log path] [--img dir] [--entry start|nnmain|addr:HEX] [--continue] [--no-init] [--trace]\n"); return 2; }
    }
    rt_log_open(logp);
    rt_log("smrpg_boot: img=%s entry=%s max-seconds=%d trace=%d", img, g_entry, g_max_seconds, g_trace);
    rt_install_veh();
    rt_fn_tables_init();
    rt_reg(RT_SENTINEL, sentinel_fn);
    svc_init();
    rt_reserve_regions();
    if (ld_load_all(img)) rt_exit(10, "load failed");
    if (g_reg_nchunks == 0) rt_log("no game library is linked (g_reg_nchunks = 0): every guest address will miss and need the interpreter");
    const char *nolift = getenv("SMRPG_NO_LIFTED");   /* differential test: run everything in the IR interpreter */
    if (nolift && *nolift == '1') rt_log("SMRPG_NO_LIFTED=1: lifted functions are not registered, the interpreter runs all guest code");
    else for (uint32_t i = 0; i < g_reg_nchunks; i++) g_reg_chunks[i]();
    rt_log("registered %u generated chunks", g_reg_nchunks);

    CreateThread(NULL, 0, watchdog_main, (LPVOID)(uintptr_t)((DWORD)g_max_seconds * 1000), 0, NULL);
    HANDLE bt = CreateThread(NULL, 64u << 20, boot_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!bt) rt_exit(10, "cannot create boot thread");
    WaitForSingleObject(bt, INFINITE);
    rt_exit(g_first_service ? 0 : 1, g_first_service ? "boot thread ended after the first service request" : "boot thread ended before any nn service request");
    return 0;
}
